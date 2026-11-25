// SPDX-License-Identifier: GPL-2.0-only
/*
 * iOS 27's System volume stays sealed as Apple shipped it. The image builder puts the plain arm64 translation
 * of each of its Mach-O files on Data, at /private/var/Switch/pretranslated/<System path>, and the tree's index
 * (kits/SwitchPretranslated) lists each file's System and Data file ids and the tree's directory ids. A lookup that
 * ends on a listed System file returns its Data copy, so exec and dyld map code that needs no translation at page-in.
 *
 * The tree is captured once, before launchd: its root vnode is kept and its index is read through it, so renaming or
 * replacing /private/var/Switch later changes nothing. The copy is found by its file id, never by a path, and only if
 * it is a regular file whose parent is one of the tree's directories. The boot package lists the CDHashes of the tree's
 * executables in /chosen/switch-pretranslated-cdhashes (and carries them in its static trust cache). Anything wrong
 * turns the redirect off with one log line: the original files then run translated at load time, as without it.
 *
 * Nothing on the device changes the tree, root included: the MAC check wrappers (security/mac_vfs.c, mac_file.c) ask
 * switch_pretranslated_refuse() before any policy, and it refuses with EPERM (one rate-limited log line) whatever
 * switch_pt_denied() says touches a vnode of the tree, the tree's root and its index among them. Only a vnode on the
 * Data mount costs a file id lookup; the hot paths of every other volume compare one pointer.
 *
 * The staged system apps are System files no lookup reaches: installd verifies Apple's bytes against their detached
 * signatures and copies them into app containers. Their translations are in the tree all the same, and the tree's
 * .originals pairs each original CDHash with its translation's, so an image mapped from such a copy takes its code
 * from the translation instead (switch_pretranslated_translation).
 *
 * A deploy keeps Data, and so the tree, but its boot package may translate a file differently (an edit of it): the boot
 * package names each such file, by System path and CDHash, in /chosen (kits/SwitchPretranslated
 * switch_pretranslated_updates.h), and the deploy writes its bytes under Switch/pretranslated-updates. Before the
 * tables are built, each update whose CDHash the boot package pins takes over its file's index entries
 * (switch_pt_apply_updates); the tree's copy stays protected. The mapping comes from the boot package, not from Data,
 * and the bytes have to be signed as that CDHash, checked here before an update stands, so whoever wrote them (a Mac
 * writes the user that mounted the card, uid 501, iOS's mobile) can serve nothing else. The kernel makes each update and its directory root's before it
 * stands, as the install makes the tree's. An update that is missing or wrong is skipped with one log line and its
 * file stays the tree's; without the /chosen list nothing here runs.
 */
#if CONFIG_A64X
#include <sys/param.h>
#include <sys/file_internal.h>
#include <sys/mman.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <sys/ubc_internal.h>
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <mach/machine.h>
#include <os/atomic_private.h>
#include <pexpert/device_tree.h>
#include <libkern/crypto/sha2.h>

#include <switch/auth_fs.h>
#include <switch/pretranslated.h>

#include "../../../kits/SwitchPretranslated/switch_pretranslated_updates.h"

#define SWITCH_PT_TREE           "Switch/pretranslated"
#define SWITCH_PT_UPDATES_DIR    "Switch/pretranslated-updates"
#define SWITCH_PT_UPDATES_LIST   "switch-pretranslated-updates"
#define SWITCH_PT_INDEX          ".index"
#define SWITCH_PT_LOOKUP         (VNODE_LOOKUP_NOFOLLOW_ANY | VNODE_LOOKUP_NOCROSSMOUNT)
#define SWITCH_PT_TRUST_PROPERTY "switch-pretranslated-cdhashes"
#define SWITCH_PT_MAX_SIGNATURE  (40u << 20)

bool switch_pretranslated_on = false;

static struct {
    mount_t system;
    mount_t data;
    vnode_t root;
    uint8_t *raw;
    size_t raw_size;
    void *storage;
    size_t storage_size;
    struct switch_pt_tables tables;
    struct switch_cdhash_list trusted;
    uint8_t *originals_raw;
    size_t originals_size;
    struct switch_pt_originals originals;
    uint32_t *original_files;
    /* The updates applied: each one's System file, the tree copy it replaced (kept protected) and its directory. */
    uint64_t update_systems[SWITCH_PT_MAX_UPDATES];
    uint64_t replaced[SWITCH_PT_MAX_UPDATES];
    uint64_t update_dirs[SWITCH_PT_MAX_UPDATES];
    uint32_t updates;
} switch_pt;

unsigned int hash_string(const char *cp, int len);

static uint32_t switch_pt_name_hash(const char *name, uint32_t length)
{
    return hash_string(name, (int)length);
}

static bool switch_pt_log_allowed(void)
{
    static uint64_t window;
    static uint32_t lines;
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &now);
    const uint64_t second = now / NSEC_PER_SEC;
    if (os_atomic_load(&window, relaxed) != second) {
        os_atomic_store(&window, second, relaxed);
        os_atomic_store(&lines, 0, relaxed);
    }
    return os_atomic_inc_orig(&lines, relaxed) < 4;
}

static int switch_pt_owned_by_root(vnode_t vp, enum vtype type, vfs_context_t ctx, uint64_t *id, off_t *size)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_data_size);
    int error = vnode_getattr(vp, &va, ctx);
    if (error) {
        return error;
    }
    if (vnode_vtype(vp) != type || !VATTR_IS_SUPPORTED(&va, va_fileid) || va.va_uid != 0 || va.va_gid != 0 ||
        (va.va_mode & (S_IWGRP | S_IWOTH))) {
        return EPERM;
    }
    *id = va.va_fileid;
    if (size) {
        *size = (off_t)va.va_data_size;
    }
    return 0;
}

/* A root-owned regular file of the tree, read whole into *raw (kalloc_data, *size bytes, the caller's to free). */
static int switch_pt_read_tree_file(vnode_t tree, const char *name, size_t min, size_t max, vfs_context_t ctx,
                                    uint8_t **raw, size_t *size, uint64_t *id)
{
    vnode_t file = NULLVP;
    off_t length = 0;
    int error = vnode_lookupat(name, SWITCH_PT_LOOKUP, &file, ctx, tree);
    if (!error && vnode_mount(file) != switch_pt.data) {
        error = EXDEV;
    }
    if (!error) {
        error = switch_pt_owned_by_root(file, VREG, ctx, id, &length);
    }
    if (!error && (length < (off_t)min || (uint64_t)length > max)) {
        error = EFBIG;
    }
    if (!error) {
        int resid = 0;
        *raw = kalloc_data((vm_size_t)length, Z_WAITOK);
        *size = (size_t)length;
        error = vn_rdwr(UIO_READ, file, (caddr_t)*raw, (int)length, 0, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(ctx), &resid, current_proc());
        if (!error && resid) {
            error = EIO;
        }
    }
    if (file) {
        vnode_put(file);
    }
    return error;
}

static int switch_pt_read_index(vnode_t tree, vfs_context_t ctx, uint64_t *index_id)
{
    return switch_pt_read_tree_file(tree, SWITCH_PT_INDEX, SWITCH_PT_HEADER, SWITCH_PT_MAX_INDEX, ctx, &switch_pt.raw,
                                    &switch_pt.raw_size, index_id);
}

static void switch_pt_release_originals(void)
{
    if (switch_pt.original_files) {
        kfree_data(switch_pt.original_files, (size_t)switch_pt.originals.count * sizeof(uint32_t));
    }
    if (switch_pt.originals_raw) {
        kfree_data(switch_pt.originals_raw, switch_pt.originals_size);
    }
    switch_pt.original_files = NULL;
    switch_pt.originals_raw = NULL;
    switch_pt.originals_size = 0;
    bzero(&switch_pt.originals, sizeof(switch_pt.originals));
}

/* The tree's .originals, protected as its index is; a tree made before there was one has none. */
static int switch_pt_read_originals(vnode_t tree, vfs_context_t ctx)
{
    uint64_t id = 0;
    int error = switch_pt_read_tree_file(tree, SWITCH_PT_ORIGINALS_NAME, SWITCH_PT_ORIGINALS_HEADER,
                                         SWITCH_PT_MAX_ORIGINALS_BYTES, ctx, &switch_pt.originals_raw,
                                         &switch_pt.originals_size, &id);
    if (error == ENOENT) {
        return 0;
    }
    enum switch_pt_error parsed = SWITCH_PT_OK;
    if (!error) {
        parsed = switch_pt_originals_parse(switch_pt.originals_raw, switch_pt.originals_size, &switch_pt.originals);
    }
    if (!error && parsed == SWITCH_PT_OK && switch_pt.originals.count) {
        switch_pt.original_files =
            kalloc_data((size_t)switch_pt.originals.count * sizeof(uint32_t), Z_WAITOK | Z_ZERO | Z_NOFAIL);
        parsed = switch_pt_originals_link(&switch_pt.tables.index, &switch_pt.originals, switch_pt.original_files);
    }
    if (!error && parsed == SWITCH_PT_OK && !switch_pt_map_put(&switch_pt.tables.data, id, SWITCH_PT_PROTECTED)) {
        parsed = SWITCH_PT_DUPLICATE_ID;
    }
    if (!error && parsed != SWITCH_PT_OK) {
        error = EINVAL;
    }
    if (error) {
        switch_pt_release_originals();
    }
    return error;
}

static int switch_pt_read_trust(void)
{
    switch_pt.trusted = switch_chosen_cdhashes(SWITCH_PT_TRUST_PROPERTY);
    return switch_pt.trusted.count ? 0 : ENOENT;
}

/* Root:wheel, writable by its owner alone: as the install leaves the tree, for an entry a deploy wrote. */
static int switch_pt_adopt(vnode_t vp, vfs_context_t ctx)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    int error = vnode_getattr(vp, &va, ctx);
    if (error) {
        return error;
    }
    const mode_t mode = (mode_t)(va.va_mode & 07777 & ~(S_ISUID | S_ISGID | S_IWGRP | S_IWOTH));
    if (va.va_uid == 0 && va.va_gid == 0 && (va.va_mode & 07777) == mode) {
        return 0;
    }
    VATTR_INIT(&va);
    VATTR_SET(&va, va_uid, 0);
    VATTR_SET(&va, va_gid, 0);
    VATTR_SET(&va, va_mode, mode);
    return vnode_setattr(vp, &va, ctx);
}

#define SWITCH_PT_UPDATE_HEAD (32u << 10)

static int switch_pt_read_at(vnode_t vp, void *buffer, uint32_t size, off_t offset, vfs_context_t ctx)
{
    int resid = 0;
    int error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)size, offset, UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(ctx), &resid, current_proc());
    return error ? error : (resid ? EIO : 0);
}

/* Whether the update's SHA-256 CodeDirectory hashes to the CDHash the boot package names for it. */
static bool switch_pt_update_signed(vnode_t vp, off_t size, const uint8_t *cdhash, vfs_context_t ctx)
{
    const uint32_t head_size = size < (off_t)SWITCH_PT_UPDATE_HEAD ? (uint32_t)size : SWITCH_PT_UPDATE_HEAD;
    uint8_t *head = kalloc_data(head_size, Z_WAITOK);
    uint8_t *blob = NULL;
    uint32_t offset = 0, length = 0, cd = 0, cd_length = 0;
    bool signed_as = false;
    if (head && !switch_pt_read_at(vp, head, head_size, 0, ctx) &&
        switch_pt_macho_signature(head, head_size, &offset, &length) && (off_t)offset + length <= size &&
        (blob = kalloc_data(length, Z_WAITOK)) != NULL && !switch_pt_read_at(vp, blob, length, offset, ctx) &&
        switch_pt_code_directory(blob, length, &cd, &cd_length)) {
        uint8_t digest[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha;
        SHA256_Init(&sha);
        SHA256_Update(&sha, blob + cd, cd_length);
        SHA256_Final(digest, &sha);
        signed_as = memcmp(digest, cdhash, SWITCH_PT_CDHASH) == 0;
    }
    if (blob) {
        kfree_data(blob, length);
    }
    if (head) {
        kfree_data(head, head_size);
    }
    return signed_as;
}

static bool switch_pt_data_id_used(const struct switch_pt_index *index, uint64_t id)
{
    for (uint32_t i = 0; i < index->count; ++i) {
        if (switch_pt_file_at(index, i).data_id == id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < index->dirs; ++i) {
        if (switch_pt_directory_at(index, i) == id) {
            return true;
        }
    }
    for (uint32_t i = 0; i < switch_pt.updates; ++i) {
        if (switch_pt.replaced[i] == id) {
            return true;
        }
    }
    return false;
}

/* One update: its System file, its copy under the update tree, and the tree's entries pointed at the copy. */
static const char *switch_pt_apply_update(vnode_t updates, const struct switch_pt_index *index,
                                          const struct switch_pt_update *update, vfs_context_t ctx)
{
    if (!switch_cdhash_listed(&switch_pt.trusted, update->cdhash)) {
        return "CDHash not pinned";
    }
    vnode_t system = NULLVP, copy = NULLVP, parent = NULLVP;
    const char *failed = NULL;
    struct vnode_attr va;
    if (vnode_lookup(update->path, VNODE_LOOKUP_NOFOLLOW_ANY, &system, ctx) ||
        vnode_mount(system) != switch_pt.system || !vnode_isreg(system)) {
        failed = "System file";
    }
    uint64_t system_id = 0;
    if (!failed) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (vnode_getattr(system, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid)) {
            failed = "System file id";
        }
        system_id = va.va_fileid;
    }
    for (uint32_t i = 0; !failed && i < switch_pt.updates; ++i) {
        if (switch_pt.update_systems[i] == system_id) {
            failed = "a hard link of an update already applied";
        }
    }
    if (!failed && (vnode_lookupat(switch_pt_update_relative(update), SWITCH_PT_LOOKUP, &copy, ctx, updates) ||
                    vnode_mount(copy) != switch_pt.data || !vnode_isreg(copy) || vnode_isnamedstream(copy))) {
        failed = "update file";
    }
    uint64_t copy_id = 0, parent_id = 0;
    if (!failed) {
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_parentid);
        VATTR_WANTED(&va, va_nlink);
        VATTR_WANTED(&va, va_data_size);
        if (vnode_getattr(copy, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid) ||
            !VATTR_IS_SUPPORTED(&va, va_parentid) || !VATTR_IS_SUPPORTED(&va, va_nlink) ||
            !VATTR_IS_SUPPORTED(&va, va_data_size) || va.va_nlink != 1) {
            failed = "update file ids";
        }
        copy_id = va.va_fileid;
        parent_id = va.va_parentid;
    }
    if (!failed && !switch_pt_update_signed(copy, (off_t)va.va_data_size, update->cdhash, ctx)) {
        failed = "not signed as its CDHash";
    }
    if (!failed && switch_pt_data_id_used(index, copy_id)) {
        failed = "update file id";
    }
    if (!failed &&
        ((parent = vnode_getparent(copy)) == NULLVP || switch_pt_adopt(parent, ctx) || switch_pt_adopt(copy, ctx))) {
        failed = "update ownership";
    }
    uint64_t replaced = 0;
    if (!failed && !switch_pt_index_point(switch_pt.raw, index, system_id, copy_id, update->cdhash, &replaced)) {
        failed = "not in the tree";
    }
    if (!failed) {
        switch_pt.update_systems[switch_pt.updates] = system_id;
        switch_pt.replaced[switch_pt.updates] = replaced;
        switch_pt.update_dirs[switch_pt.updates] = parent_id;
        ++switch_pt.updates;
    }
    if (parent) {
        vnode_put(parent);
    }
    if (copy) {
        vnode_put(copy);
    }
    if (system) {
        vnode_put(system);
    }
    return failed;
}

/* The boot package's updates, applied to the parsed index before its tables are built. Nothing here can turn the tree
 * off: an update tree that cannot be used leaves every file the tree's. */
static void switch_pt_apply_updates(vnode_t tree, const struct switch_pt_index *index, vfs_context_t ctx)
{
    DTEntry chosen;
    const void *bytes = NULL;
    unsigned int size = 0;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, SWITCH_PT_UPDATES_LIST, &bytes, &size) != kSuccess) {
        return;
    }
    struct switch_pt_updates list;
    const enum switch_pt_error parsed = switch_pt_updates_parse((const uint8_t *)bytes, size, &list);
    if (parsed != SWITCH_PT_OK) {
        printf("SwitchPretranslated: no updates (/chosen/" SWITCH_PT_UPDATES_LIST ": %d)\n", (int)parsed);
        return;
    }
    vnode_t data_switch = vnode_getparent(tree), updates = NULLVP;
    uint64_t id = 0;
    int error = data_switch ? switch_pt_owned_by_root(data_switch, VDIR, ctx, &id, NULL) : ENOENT;
    if (!error) {
        error = vnode_lookupat("pretranslated-updates", SWITCH_PT_LOOKUP, &updates, ctx, data_switch);
    }
    if (!error && (vnode_mount(updates) != switch_pt.data || !vnode_isdir(updates))) {
        error = ENOTDIR;
    }
    if (!error) {
        error = switch_pt_adopt(updates, ctx);
    }
    if (error) {
        printf("SwitchPretranslated: %u updates named, none applied (/private/var/" SWITCH_PT_UPDATES_DIR ": %d)\n",
               list.count, error);
    }
    for (uint32_t i = 0; !error && i < list.count; ++i) {
        const struct switch_pt_update update = switch_pt_update_at(&list, i);
        const char *failed = switch_pt_apply_update(updates, index, &update, ctx);
        printf("SwitchPretranslated: %s %s\n", update.path, failed ? failed : "from the deploy's update");
    }
    if (updates) {
        vnode_put(updates);
    }
    if (data_switch) {
        vnode_put(data_switch);
    }
}

static const char *switch_pt_build(vnode_t tree, vfs_context_t ctx, int *error)
{
    uint64_t tree_id = 0, index_id = 0;
    struct switch_pt_index index;
    if ((*error = switch_pt_owned_by_root(tree, VDIR, ctx, &tree_id, NULL))) {
        return "tree";
    }
    if ((*error = switch_pt_read_index(tree, ctx, &index_id))) {
        return "index";
    }
    enum switch_pt_error parsed = switch_pt_index_parse(switch_pt.raw, switch_pt.raw_size, &index);
    if (parsed != SWITCH_PT_OK) {
        *error = (int)parsed;
        return "index contents";
    }
    switch_pt_apply_updates(tree, &index, ctx);
    /* the index and .originals: protected files no record lists; per update, the copy it replaced and its directory */
    const uint32_t extra = 2 + 2 * switch_pt.updates;
    switch_pt.storage_size = switch_pt_tables_bytes(&index, extra);
    switch_pt.storage = kalloc_data(switch_pt.storage_size, Z_WAITOK | Z_ZERO);
    parsed = switch_pt_tables_build(&switch_pt.tables, &index, extra, switch_pt.storage, switch_pt.storage_size,
                                    switch_pt_name_hash);
    if (parsed == SWITCH_PT_OK && !switch_pt_map_put(&switch_pt.tables.data, index_id, SWITCH_PT_PROTECTED)) {
        parsed = SWITCH_PT_DUPLICATE_ID;
    }
    for (uint32_t i = 0; parsed == SWITCH_PT_OK && i < switch_pt.updates; ++i) {
        if (!switch_pt_map_put(&switch_pt.tables.data, switch_pt.replaced[i], SWITCH_PT_PROTECTED) ||
            (!switch_pt_map_get(&switch_pt.tables.data, switch_pt.update_dirs[i], NULL) &&
             !switch_pt_map_put(&switch_pt.tables.data, switch_pt.update_dirs[i], SWITCH_PT_DIRECTORY))) {
            parsed = SWITCH_PT_DUPLICATE_ID;
        }
    }
    uint32_t root_kind = 0;
    if (parsed == SWITCH_PT_OK &&
        (!switch_pt_map_get(&switch_pt.tables.data, tree_id, &root_kind) || root_kind != SWITCH_PT_DIRECTORY)) {
        parsed = SWITCH_PT_DIRECTORY_BAD;
    }
    if (parsed != SWITCH_PT_OK) {
        *error = (int)parsed;
        return "index ids";
    }
    return NULL;
}

static void switch_pt_release(void)
{
    switch_pt_release_originals();
    if (switch_pt.storage) {
        kfree_data(switch_pt.storage, switch_pt.storage_size);
    }
    if (switch_pt.raw) {
        kfree_data(switch_pt.raw, switch_pt.raw_size);
    }
    if (switch_pt.root) {
        vnode_rele(switch_pt.root);
    }
    bzero(&switch_pt, sizeof(switch_pt));
}

/* Before launchd, once Data is mounted at /private/var: nothing else runs yet, so its path is still the one mounted. */
void switch_pretranslated_capture(void)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t data_root = NULLVP, tree = NULLVP;
    const char *failed = NULL;
    int error = vnode_lookup("/private/var", 0, &data_root, ctx);
    if (error || !vnode_isvroot(data_root) || (vfs_flags(vnode_mount(data_root)) & MNT_RDONLY) ||
        vnode_mount(data_root) == rootvnode->v_mount) {
        failed = "Data mount";
        error = error ? error : EINVAL;
    }
    if (!failed && (error = switch_pt_read_trust())) {
        failed = "CDHash list";
    }
    if (!failed) {
        switch_pt.system = rootvnode->v_mount;
        switch_pt.data = vnode_mount(data_root);
        error = vnode_lookupat(SWITCH_PT_TREE, SWITCH_PT_LOOKUP, &tree, ctx, data_root);
        if (!error && vnode_mount(tree) != switch_pt.data) {
            error = EXDEV;
        }
        failed = error ? "tree" : switch_pt_build(tree, ctx, &error);
    }
    if (!failed && (error = vnode_ref(tree))) {
        failed = "tree reference";
    }
    if (!failed && (error = switch_pt_read_originals(tree, ctx))) {
        printf("SwitchPretranslated: installed system apps run translated at load time (%s: %d)\n",
               SWITCH_PT_ORIGINALS_NAME, error);
    }
    if (!failed) {
        switch_pt.root = tree;
    }
    if (tree) {
        vnode_put(tree);
    }
    if (data_root) {
        vnode_put(data_root);
    }
    if (failed) {
        printf("SwitchPretranslated: off (%s: %d); the System files run translated at load time\n", failed, error);
        switch_pt_release();
        return;
    }
    const fsid_t fsid = vfs_statfs(switch_pt.data)->f_fsid;
    printf("SwitchPretranslated: %u files from /private/var/" SWITCH_PT_TREE " (fsid %x:%x)\n",
           switch_pt.tables.index.count, (uint32_t)fsid.val[0], (uint32_t)fsid.val[1]);
    os_atomic_store(&switch_pretranslated_on, true, release);
}

static int switch_pt_copy(uint64_t data_id, vfs_context_t ctx, vnode_t *copy)
{
    struct vnode_attr va;
    uint32_t parent_kind = 0, listed = 0;
    int error = VFS_VGET(switch_pt.data, (ino64_t)data_id, copy, ctx);
    if (error) {
        return error;
    }
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_parentid);
    error = vnode_getattr(*copy, &va, ctx);
    if (!error && (vnode_mount(*copy) != switch_pt.data || !vnode_isreg(*copy) || vnode_isnamedstream(*copy) ||
                   !VATTR_IS_SUPPORTED(&va, va_parentid) || va.va_fileid != data_id ||
                   !switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, &listed) || listed >= SWITCH_PT_PROTECTED ||
                   !switch_pt_map_get(&switch_pt.tables.data, va.va_parentid, &parent_kind) ||
                   parent_kind != SWITCH_PT_DIRECTORY)) {
        error = EPERM;
    }
    if (error) {
        vnode_put(*copy);
        *copy = NULLVP;
    }
    return error;
}

/* The end of a lookup (lookup(), with an iocount on ni_vp): a listed System file becomes its Data copy, named as the
 * System file is (its parent and name), so paths built from it are the System path. */
void switch_pretranslated_redirect(struct nameidata *ndp)
{
    struct componentname *cnp = &ndp->ni_cnd;
    vnode_t vp = ndp->ni_vp;
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || vp == NULLVP || vp->v_type != VREG ||
        vp->v_mount != switch_pt.system || cnp->cn_nameiop != LOOKUP || (cnp->cn_flags & CN_WANTSRSRCFORK) ||
        vnode_isnamedstream(vp) || cnp->cn_namelen <= 0) {
        return;
    }
    const uint32_t length = (uint32_t)cnp->cn_namelen;
    const uint32_t hash = cnp->cn_hash ? cnp->cn_hash : hash_string(cnp->cn_nameptr, cnp->cn_namelen);
    if (!switch_pt_name_listed(&switch_pt.tables, hash, cnp->cn_nameptr, length)) {
        return;
    }
    vfs_context_t ctx = cnp->cn_context ? cnp->cn_context : vfs_context_current();
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    if (vnode_getattr(vp, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_fileid)) {
        return;
    }
    const int64_t file = switch_pt_system_file(&switch_pt.tables, va.va_fileid, hash, cnp->cn_nameptr, length);
    if (file < 0) {
        return;
    }
    const struct switch_pt_file listed = switch_pt_file_at(&switch_pt.tables.index, (uint32_t)file);
    vnode_t copy = NULLVP;
    int error = switch_pt_copy(listed.data_id, ctx, &copy);
    if (error) {
        if (switch_pt_log_allowed()) {
            printf("SwitchPretranslated: %s stays untranslated: %d\n", listed.path, error);
        }
        return;
    }
    if (copy->v_parent != vp->v_parent || copy->v_name == NULL) {
        vnode_t parent = vnode_getparent(vp);
        if (parent != NULLVP) {
            vnode_update_identity(copy, parent, cnp->cn_nameptr, cnp->cn_namelen, hash,
                                  VNODE_UPDATE_PARENT | (copy->v_name ? 0 : VNODE_UPDATE_NAME));
            vnode_put(parent);
        }
    }
    vnode_put(vp);
    ndp->ni_vp = copy;
}

/* The System file a regular file of the Data mount stands in for: its id (0 for none), and the file's own id. known is
 * the caller's attributes, which may already hold the file id. */
static uint64_t switch_pt_system_id_of(vnode_t vp, const struct vnode_attr *known, uint64_t *data_id)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || vp == NULLVP || vp->v_mount != switch_pt.data ||
        vp->v_type != VREG) {
        return 0;
    }
    if (known != NULL && VATTR_IS_SUPPORTED(known, va_fileid)) {
        *data_id = known->va_fileid;
    } else {
        struct vnode_attr id;
        VATTR_INIT(&id);
        VATTR_WANTED(&id, va_fileid);
        if (vnode_getattr(vp, &id, vfs_context_kernel()) || !VATTR_IS_SUPPORTED(&id, va_fileid)) {
            return 0;
        }
        *data_id = id.va_fileid;
    }
    return switch_pt_system_id(&switch_pt.tables, *data_id);
}

/* stat and getattrlist: the System file's device and id, so dev and ino stay one file's (identity.c says why statfs
 * reports the System volume). Not when the caller asks for the real device; the kernel's own vnode_getattr keeps the
 * Data ids the tables are keyed by. */
void switch_pretranslated_identity(vnode_t vp, struct vnode_attr *va)
{
    if ((va->va_vaflags & VA_REALFSID) || !(VATTR_IS_ACTIVE(va, va_fsid) || VATTR_IS_ACTIVE(va, va_fsid64) ||
                                            VATTR_IS_SUPPORTED(va, va_fileid) || VATTR_IS_SUPPORTED(va, va_linkid))) {
        return;
    }
    uint64_t data_id = 0;
    const uint64_t system_id = switch_pt_system_id_of(vp, va, &data_id);
    if (system_id == 0) {
        return;
    }
    const fsid_t system = vfs_statfs(switch_pt.system)->f_fsid;
    if (VATTR_IS_ACTIVE(va, va_fsid)) {
        VATTR_RETURN(va, va_fsid, (uint32_t)system.val[0]);
    }
    if (VATTR_IS_ACTIVE(va, va_fsid64)) {
        VATTR_RETURN(va, va_fsid64, system);
    }
    if (VATTR_IS_SUPPORTED(va, va_fileid)) {
        va->va_fileid = system_id;
    }
    if (VATTR_IS_SUPPORTED(va, va_linkid) && va->va_linkid == data_id) {
        va->va_linkid = system_id;
    }
}

mount_t switch_pretranslated_system_volume(vnode_t vp)
{
    uint64_t data_id = 0;
    return switch_pt_system_id_of(vp, NULL, &data_id) ? switch_pt.system : NULL;
}

static const char *switch_pt_op_name(enum switch_pt_op op)
{
    static const char *const names[SWITCH_PT_OPS] = {
        [SWITCH_PT_OPEN] = "write open",
        [SWITCH_PT_TRUNCATE] = "truncate",
        [SWITCH_PT_SETATTR] = "attribute change",
        [SWITCH_PT_SETEXTATTR] = "xattr write",
        [SWITCH_PT_DELETEEXTATTR] = "xattr removal",
        [SWITCH_PT_UNLINK] = "unlink",
        [SWITCH_PT_RENAME] = "rename",
        [SWITCH_PT_LINK] = "link",
        [SWITCH_PT_CREATE] = "create",
        [SWITCH_PT_EXCHANGEDATA] = "exchangedata",
        [SWITCH_PT_CLONE] = "clone",
        [SWITCH_PT_COPYFILE] = "copyfile",
        [SWITCH_PT_FSCTL] = "fsctl or ioctl",
        [SWITCH_PT_MMAP] = "shared writable mapping",
        [SWITCH_PT_MOUNT] = "mount",
        [SWITCH_PT_GRAFT] = "graft",
    };
    return op < SWITCH_PT_OPS ? names[op] : "change";
}

/* Membership is decided from the vnode's own identity, not by walking paths. A vnode off the Data mount is never in
 * the tree (one pointer, no I/O): every other volume's hot path stops here, so ordinary files are untouched. For a
 * vnode on the Data mount the file id comes from its in-core node (no device read for a resident vnode); if even that
 * cannot be read the write is refused, since the id can only be missing for a vnode we failed to identify. */
static bool switch_pt_in_tree(vnode_t vp)
{
    if (vp == NULLVP || vp->v_mount != switch_pt.data) {
        return false;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    if (vnode_getattr(vp, &va, vfs_context_kernel()) || !VATTR_IS_SUPPORTED(&va, va_fileid)) {
        return true;
    }
    return switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, NULL);
}

int switch_pretranslated_refuse(enum switch_pt_op op, vnode_t v0, vnode_t v1, vnode_t v2, vnode_t v3, uint32_t intent)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || !switch_pt_denied(op, SWITCH_PT_ANY_VNODE, intent)) {
        return 0;
    }
    const vnode_t vnodes[4] = {v0, v1, v2, v3};
    uint32_t in_tree = 0;
    for (uint32_t i = 0; i < 4 && !switch_pt_denied(op, in_tree, intent); ++i) {
        if (switch_pt_denied(op, SWITCH_PT_VNODE(i), intent) && switch_pt_in_tree(vnodes[i])) {
            in_tree |= SWITCH_PT_VNODE(i);
        }
    }
    if (!switch_pt_denied(op, in_tree, intent)) {
        return 0;
    }
    if (switch_pt_log_allowed()) {
        proc_t p = current_proc();
        printf("SwitchPretranslated: refused %s in /private/var/" SWITCH_PT_TREE " by %s[%d]\n", switch_pt_op_name(op),
               p ? proc_best_name(p) : "kernel", p ? proc_getpid(p) : 0);
    }
    return EPERM;
}

static bool switch_pt_signed_as(vnode_t vp, const uint8_t *cdhash)
{
    bool found = false;
    vnode_lock_spin(vp);
    for (const struct cs_blob *blob = ubc_get_cs_blobs(vp); blob && !found; blob = blob->csb_next) {
        found = memcmp(blob->csb_cdhash, cdhash, SWITCH_PT_CDHASH) == 0;
    }
    vnode_unlock(vp);
    return found;
}

/*
 * The System files the tree stands in for are on the sealed System volume; their Data copies are authenticated by
 * the boot package's CDHash list instead (auth_fs.c). Decided from the vnode alone, never from the path it was found
 * by.
 */
bool switch_pretranslated_authentic(vnode_t vp)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || vp == NULLVP || vp->v_mount != switch_pt.data ||
        !vnode_isreg(vp)) {
        return false;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_fileid);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_gid);
    VATTR_WANTED(&va, va_mode);
    if (vnode_getattr(vp, &va, vfs_context_kernel()) || !VATTR_ALL_SUPPORTED(&va)) {
        return false;
    }
    uint32_t file = SWITCH_PT_PROTECTED;
    if (!switch_pt_map_get(&switch_pt.tables.data, va.va_fileid, &file) || file >= SWITCH_PT_PROTECTED) {
        return false;
    }
    const uint8_t *cdhash = switch_pt_file_at(&switch_pt.tables.index, file).cdhash;
    const struct switch_pt_trust_facts facts = {
        .listed = true,
        .regular = true,
        .uid = va.va_uid,
        .gid = va.va_gid,
        .mode = va.va_mode,
        .cdhash_pinned = switch_cdhash_listed(&switch_pt.trusted, cdhash),
        .cdhash_matches = switch_pt_signed_as(vp, cdhash),
    };
    const bool authentic = switch_pt_trusted(&facts);
    if (!authentic && switch_pt_log_allowed()) {
        printf("SwitchPretranslated: file %llu is not authenticated (%u:%u %o, CDHash pinned %d matches %d)\n",
               (unsigned long long)va.va_fileid, facts.uid, facts.gid, facts.mode, facts.cdhash_pinned,
               facts.cdhash_matches);
    }
    return authentic;
}

/* The file's embedded signature, as exec and F_ADDFILESIGS load one: AMFI admits it by its trust-cached CDHash. */
static int switch_pt_load_signature(vnode_t vp, off_t slice_offset, off_t signature_offset, uint32_t signature_size)
{
    if (ubc_cs_blob_get(vp, CPU_TYPE_ANY, CPU_SUBTYPE_ANY, slice_offset)) {
        return 0;
    }
    if (signature_size == 0 || signature_size > SWITCH_PT_MAX_SIGNATURE) {
        return EBADMACHO;
    }
    vm_offset_t addr = 0;
    vm_size_t size = signature_size;
    if (ubc_cs_blob_allocate(&addr, &size) != KERN_SUCCESS) {
        return ENOMEM;
    }
    int resid = 0;
    int error = vn_rdwr(UIO_READ, vp, (caddr_t)addr, (int)signature_size, signature_offset, UIO_SYSSPACE, 0,
                        vfs_context_ucred(vfs_context_kernel()), &resid, current_proc());
    if (!error && resid) {
        error = EBADMACHO;
    }
    struct cs_blob *blob = NULL;
    if (!error) {
        error = ubc_cs_blob_add(vp, proc_platform(current_proc()), CPU_TYPE_ANY, CPU_SUBTYPE_ANY, slice_offset, &addr,
                                signature_size, NULL, 0, &blob, CS_BLOB_ADD_ALLOW_MAIN_BINARY);
    }
    if (addr) {
        ubc_cs_blob_deallocate(addr, size);
    }
    return error;
}

vnode_t switch_pretranslated_translation(vnode_t vp, off_t slice_offset, off_t signature_offset,
                                         uint32_t signature_size)
{
    if (!os_atomic_load(&switch_pretranslated_on, acquire) || !switch_pt.originals.count || vp == NULLVP ||
        vp->v_mount != switch_pt.data) {
        return NULLVP;
    }
    const struct cs_blob *blob = ubc_cs_blob_get(vp, CPU_TYPE_ANY, CPU_SUBTYPE_ANY, slice_offset);
    const int64_t file =
        blob ? switch_pt_original_file(&switch_pt.originals, switch_pt.original_files, blob->csb_cdhash) : -1;
    if (file < 0) {
        return NULLVP;
    }
    const struct switch_pt_file listed = switch_pt_file_at(&switch_pt.tables.index, (uint32_t)file);
    vnode_t copy = NULLVP;
    int error = switch_pt_copy(listed.data_id, vfs_context_kernel(), &copy);
    if (!error) {
        error = switch_pt_load_signature(copy, slice_offset, signature_offset, signature_size);
    }
    if (!error && !switch_pretranslated_authentic(copy)) {
        error = EPERM;
    }
    if (error) {
        if (switch_pt_log_allowed()) {
            printf("SwitchPretranslated: %s stays translated at load time: %d\n", listed.path, error);
        }
        if (copy) {
            vnode_put(copy);
        }
        return NULLVP;
    }
    return copy;
}

int switch_pretranslated_mmap_check(struct fileglob *fg, int flags, int prot, int maxprot)
{
    if (FILEGLOB_DTYPE(fg) == DTYPE_VNODE && (flags & MAP_SHARED) &&
        ((prot & PROT_WRITE) || (maxprot & VM_PROT_WRITE))) {
        return switch_pretranslated_check(SWITCH_PT_MMAP, (vnode_t)fg_get_data(fg), NULL, NULL, NULL, SWITCH_PT_WRITES);
    }
    return 0;
}
#endif
