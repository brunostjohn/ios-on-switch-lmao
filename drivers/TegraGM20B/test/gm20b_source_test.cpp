// SPDX-License-Identifier: MIT
// Guards on how TegraGM20B's kernel code uses what gm20b_pool_policy_test and gm20b_push_scan_test exercise, and on the
// pool's zeroing, which no host model runs (it lives in nvkm's shim and gpu_scrub.c's channel):
//  - the user client decides a coherent BO's memory through TegraGM20BPoolPolicy.h, serves a BO the pool does not from
//    a write-combined buffer of its own, never refuses a coherent BO for want of pool room, and the stats line names
//    the pool's holders;
//  - the CPU does not clear the reserved pool at boot; every page an allocation hands out is zero whether the CPU or
//    the copy engine zeroed it; the scrubber gives a range back clean only from its fence's interrupt pass, never waits
//    on the CPU, and panics with one line on a range not back in time;
//  - the user client reads pushes only through TegraGM20BPushScan.h and profiles only what it read;
//  - a fence-only present (direct scanout) copies nothing, and the debug read-back rides the frame's own present.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cctype>
#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace
{
const std::string &client()
{
	static const std::string source = [] {
		std::string units;
		for (const char *unit :
		     {"", "Pool", "Bo", "Channel", "Submit", "Dispatch", "Present", "Job", "JobSubmit"})
			units += SourceText(std::string("../TegraGM20BUserClient") + unit + ".cpp");
		return units + SourceText("../TegraGM20BUserClientInternal.h") +
		       SourceText("../SwitchGpuClientRetirement.h");
	}();
	return source;
}

const std::string &gpu()
{
	static const std::string source = [] {
		std::string units;
		for (const char *unit : {"bringup", "clock", "mem", "hang", "describe", "acquire",
					 "stats", "stats_tick", "chan", "sync", "exec", "present",
					 "present_deadline", "present_redo", "reload", "scrub", "power"})
			units += SourceText(std::string("../gpu_") + unit + ".c");
		return units + SourceText("../gpu_internal.h") + SourceText("../gpu_inline.h") +
		       SourceText("../gpu_fenced.h") + SourceText("../SwitchGpuVmMap.h") +
		       SourceText("../SwitchGpuChannelCompletion.h");
	}();
	return source;
}

// The body of the function whose definition (not a declaration of it) starts with `signature`.
std::string definition(const std::string &source, const std::string &signature)
{
	for (size_t at = source.find(signature); at != std::string::npos;
	     at = source.find(signature, at + 1)) {
		const std::string rest = source.substr(at + signature.size());
		if (rest.substr(0, rest.find('{')).find(';') != std::string::npos)
			continue;
		return rest.substr(0, rest.find("\n}\n"));
	}
	ADD_FAILURE() << "no definition of " << signature;
	return {};
}

// `text` with every run of whitespace one space (the checks read code, not its line breaks).
std::string flat(const std::string &text)
{
	std::string out;
	bool space = false;
	for (const char c : text) {
		if (std::isspace((unsigned char)c)) {
			space = true;
			continue;
		}
		if (space)
			out += ' ';
		space = false;
		out += c;
	}
	if (space)
		out += ' ';
	return out;
}

// `first` occurs in `text`, and before `second`.
void expectBefore(const std::string &text, const char *first, const char *second)
{
	const size_t a = text.find(first), b = text.find(second);
	ASSERT_NE(a, std::string::npos) << first;
	ASSERT_NE(b, std::string::npos) << second;
	EXPECT_LT(a, b) << first << " before " << second;
}
}  // namespace

TEST(GM20BPoolPolicySource, BoAllocNeverRefusesACoherentBoForPoolRoom)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPoolPolicy.h\""));
	const std::string refusal = flat(definition(
		client(),
		"const char *poolRefusal(uint64_t bytes, uint64_t held, bool compositor)"));
	EXPECT_THAT(refusal, HasSubstr("tgm_pool::refusal("));
	EXPECT_THAT(refusal, HasSubstr("compositor);"));  // the compositor keeps nvkm's floor
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(alloc, HasSubstr("refused = control ? nullptr : poolRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_THAT(alloc, HasSubstr("tgm_pool::coherentSource(refused, bo->pool_va != nullptr)"));
	// A refusal does not fail the BO: no NoMemory between the tier check and the pool allocation.
	EXPECT_THAT(Between(alloc, "if (!ret && (in->flags & TegraGM20BBoCoherent)) {",
			    "if (!ret && bo->pool_va)"),
		    Not(HasSubstr("kIOReturnNoMemory")));
	EXPECT_THAT(client(), Not(HasSubstr("GPU pool BoAlloc of %llu KiB refused")));
	// A system BO: a write-combined buffer of its own, mapped as itself. A physical view over its managed pages would set
	// their cache attribute through a device pager whose private pages reset it to cacheable whenever they are freed.
	EXPECT_THAT(Between(alloc, "} else if (!ret && !compressible) {", "if (!ret && bo->bmd) {"),
		    HasSubstr("if (system) options |= kIOMapWriteCombineCache;"));
	EXPECT_THAT(
		alloc,
		Not(HasSubstr(
			"kIOMemoryTypePhysical64) | kPoolCacheMode, TASK_NULL); IODelete(ranges")));
	EXPECT_THAT(alloc,
		    HasSubstr("if (!ret && bo->bmd) { bo->view = bo->backing_pages == bo->npages"));
}

TEST(GM20BPoolPolicySource, SystemBosGoAsCachedBosDo)
{
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	EXPECT_THAT(release, HasSubstr("if (bo->pool_va && bo->view) orphan = IONew(PoolOrphan, "
				       "1); if (orphan) {"));
	EXPECT_THAT(release, HasSubstr("!bo->system && !(bo->flags & TegraGM20BBoContiguous) &&"));
	expectBefore(release, "spareKeep(", "bo->bmd->release();");
	EXPECT_THAT(flat(definition(client(), "void releaseOrphans()")),
		    Not(HasSubstr("spareKeep")));
	// The kernel's mapping of a system BO is write-combined as its task's is: pushView reads it as a cached BO's.
	EXPECT_THAT(flat(definition(client(), "const uint32_t *AGXDeviceUserClient::pushView(")),
		    HasSubstr("if (hit) { if (hit->bo->pool_va) {"));
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::scanPushes(")),
		    HasSubstr("pushView(va, bytes, hit)"));
	EXPECT_THAT(flat(definition(client(), "int AGXDeviceUserClient::hungInterlocks(")),
		    HasSubstr("pushView(va[i], static_cast<uint64_t>(dwords[i]) * 4, hit)"));
}

// A system BO neither takes a spare nor gives its pages to the spares, which are cacheable: it is made at its own size.
// A cached BO's pages are in one state from every source: zero in DRAM, no line in the cacheable kernel alias
// (cleanZeros publishes KMA_ZERO; recycled spares become ready only after CE completion).
TEST(GM20BPoolPolicySource, SystemBosNeverUseSpares)
{
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	EXPECT_THAT(
		alloc,
		HasSubstr("if (!ret && !bo->pool_va && !compressible && !system && "
			  "!(in->flags & TegraGM20BBoContiguous)) spare = spareTake(in->bytes);"));
	EXPECT_THAT(alloc, HasSubstr("(in->flags & TegraGM20BBoContiguous) || system ? kClasses : "
				     "classUp(in->bytes);"));
	EXPECT_THAT(alloc, HasSubstr("cleanZeros(va, backing);"));
	EXPECT_THAT(flat(definition(client(), "Spare *spareMake(uint64_t bytes)")),
		    HasSubstr("cleanZeros(bmd->getBytesNoCopy(), bytes);"));
	const std::string work = flat(definition(
		client(), "void spareWork(thread_call_param_t param, thread_call_param_t)"));
    EXPECT_THAT(work, HasSubstr("switch_gpu_cached_zero(s->pas"));
    EXPECT_THAT(work, Not(HasSubstr("cleanZeros(s->bmd")));
    const std::string completed = flat(definition(client(), "void spareZeroed(void *context, int result)"));
    expectBefore(completed, "const bool keep = !result && !memoryPressure();", "p->ready[c] = s;");
	const std::string zeros =
		flat(definition(client(), "void cleanZeros(void *va, uint64_t bytes)"));
	EXPECT_THAT(zeros, HasSubstr("switch_dcache_clean_invalidate(va, bytes);"));
    EXPECT_THAT(zeros, Not(HasSubstr("bzero")));
	// A pool view's pages go back only once no task maps the view; a cached BO's buffer is a spare only once nothing
	// else holds it.
	const std::string orphans = flat(definition(client(), "void releaseOrphans()"));
	expectBefore(orphans, "if (orphan->view->getRetainCount() > 1) {",
		     "orphan->view->release();");
	EXPECT_THAT(orphans, HasSubstr("orphan->zero &&"));
	EXPECT_THAT(flat(definition(client(), "bool spareKeep(IOBufferMemoryDescriptor *bmd, "
					      "uint64_t *pas, uint64_t bytes,")),
		    HasSubstr("bmd->getRetainCount() == 1"));
	const std::string free =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boFree("));
	expectBefore(free, "unbindOverlaps(binds_[i].va, binds_[i].range, bo);", "queueZombie(bo);");
}

// The import costs (the "imports" and "slow import" lines, the refused import's region) are the debug kernel's alone:
// a release kernel neither reads the thread's CPU time nor walks the task's map.
TEST(GM20BImportCostSource, ImportCostsAreDebugOnly)
{
	const std::string stats =
		Between(gpu(), "#if TGM_STATS\n/* The \"imports\" line's histograms", "#endif\n");
	EXPECT_THAT(stats, HasSubstr("thread_get_runtime_self()"));
	EXPECT_EQ(Count(gpu(), "thread_get_runtime_self"), Count(stats, "thread_get_runtime_self"));
	const std::string header = SourceText("../TegraGM20BGpu.h");
	EXPECT_THAT(
		Between(header, "#else\nstatic inline uint64_t tgm_stats_cpu_ns(void)", "#endif"),
		HasSubstr("return 0;"));
	const std::string import =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boImport("));
	EXPECT_THAT(import, HasSubstr("#if TGM_STATS describeRegion(task_, in->address, region, "
				      "sizeof(region)); #endif"));
	EXPECT_THAT(import, HasSubstr("tgm_mem_new_waited(vm_, bo->pas, bo->npages, &bo->mem, "
				      "&cost.lock_ns)"));
	EXPECT_THAT(flat(definition(gpu(), "static int mem_new(")),
		    HasSubstr("asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;"));
}

TEST(GM20BPoolPolicySource, StatsNameThePoolHolders)
{
	const std::string stats = flat(gpu());
	EXPECT_THAT(
		stats,
		HasSubstr("tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, "
			  "&pool_comp_bytes, &pool_comps, &pool_system_bytes, &pool_systems);"));
	EXPECT_THAT(
		stats,
		HasSubstr(
			"pool %lu/%llu MiB (compositor %llu, clients \" \"%llu, orphans %llu in"));
	EXPECT_THAT(stats, HasSubstr("compressible %llu MiB in %llu;"));
}

// A Compressible BO is pool chunks under the pool's tiers, never a view a task could map, and its chunks go back only once
// its GPU memory object (and with it its comptags) is gone.
TEST(GM20BPoolPolicySource, CompressibleBosArePoolChunksNoTaskMaps)
{
	const std::string alloc =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::boAlloc("));
	const std::string chunks = Between(alloc, "if (!ret && compressible) {",
					   "if (!ret && (in->flags & TegraGM20BBoCoherent)) {");
	EXPECT_THAT(chunks,
		    HasSubstr("refused = compressibleRefusal(in->bytes, pool_bytes_, critical_);"));
	EXPECT_THAT(
		flat(definition(client(), "const char *compressibleRefusal(uint64_t bytes, "
					  "uint64_t held, bool compositor)")),
		HasSubstr("tgm_pool::compressibleRefusal(bytes, held, avail, limits, compositor)"));
	EXPECT_THAT(chunks, HasSubstr("takeChunks(in->bytes, bo->pas)"));
	EXPECT_THAT(alloc, HasSubstr("} else if (!ret && !compressible) {"));
	EXPECT_THAT(
		flat(definition(client(), "IOReturn takeChunks(uint64_t bytes, uint64_t *pas)")),
		Not(HasSubstr("poolZeroNow()")));
	const std::string release =
		flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	expectBefore(release, "tgm_mem_del(&bo->mem);",
		     "tgm_pool_free_chunks(bo->pas, bo->npages);");
	EXPECT_THAT(
		flat(definition(client(), "IOReturn AGXDeviceUserClient::clientMemoryForType(")),
		HasSubstr("bo && bo->view"));
	EXPECT_THAT(flat(definition(SourceText("../shim/linux.c"),
				    "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned "
				    "long long *pas)")),
		    HasSubstr("tgm_comp_pick_chunks(pool_map, pool_dirty, pool_npages,"));
}

TEST(GM20BPoolScrubSource, NoCpuClearOfTheReservedPool)
{
	const std::string init = definition(SourceText("../shim/xnu_glue.cpp"),
					    "extern \"C\" int nvxnu_pool_init(int *zeroed)");
	EXPECT_THAT(
		Between(init, "switch_ram_gpu_pool(&start, &end)", "IOBufferMemoryDescriptor *md"),
		Not(HasSubstr("memset")));
	EXPECT_THAT(init, HasSubstr("*zeroed = 0;"));
	EXPECT_THAT(init, HasSubstr("*zeroed = 1;"));  // the fallback still zeroes its own pages
	EXPECT_THAT(definition(SourceText("../shim/linux.c"), "int nvxnu_pool_setup(void)"),
		    HasSubstr("bitmap_set(pool_dirty, 0, pool_npages)"));
}

TEST(GM20BPoolScrubSource, EveryAllocationZeroesWhatAFreeLeftDirty)
{
	const std::string shim = SourceText("../shim/linux.c");
	for (const char *signature :
	     {"struct page *alloc_pages(gfp_t gfp, unsigned int order)",
	      "void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *handle, gfp_t "
	      "gfp,",
	      "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long align, unsigned long "
	      "long *pa)"})
		EXPECT_THAT(definition(shim, signature), HasSubstr("pool_zero_dirty("))
			<< signature;
    const std::string chunks = definition(shim, "int nvxnu_pool_alloc_chunks(unsigned long count, unsigned long long *pas)");
    EXPECT_THAT(chunks, HasSubstr("dirty > 0"));
    EXPECT_THAT(chunks, Not(HasSubstr("pool_zero_dirty(")));
    EXPECT_THAT(definition(shim, "void *nvxnu_pool_alloc_ready("), Not(HasSubstr("pool_zero_dirty(")));
	const std::string zero = definition(
		shim, "static void pool_zero_dirty(unsigned long first, unsigned long n, int all)");
	EXPECT_THAT(zero, HasSubstr("test_bit(first + k, pool_dirty)"));
	EXPECT_THAT(zero, HasSubstr("bitmap_clear(pool_dirty, first, n)"));
	// A plain free marks pages dirty; only a zeroed free (CPU or scrubber) marks them clean.
	EXPECT_THAT(definition(shim, "static void pool_free(unsigned long first, unsigned long n)"),
		    HasSubstr("bitmap_set(pool_dirty, first, n)"));
	const std::string claim = definition(shim, "unsigned long nvxnu_pool_claim_dirty(unsigned "
						   "long max_bytes, unsigned long long *pa)");
	EXPECT_THAT(claim, HasSubstr("pool_dirty[w] & ~pool_map[w]"));
	EXPECT_THAT(
		claim,
		HasSubstr(
			"bitmap_set(pool_map, first, n)"));  // claimed ranges are allocated: nobody takes them
}

// A process maps whole VM pages (16 KiB, four pool pages): every pool BO and fence slot it can map is whole VM pages,
// so its mapping never reaches another BO's, another connection's fence or nvkm's own memory.
TEST(GM20BPoolIsolationSource, CpuMappedPoolMemoryIsWholeVmPages)
{
	const std::string alloc = flat(definition(
		gpu(), "void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)"));
	EXPECT_THAT(alloc, HasSubstr("nvxnu_pool_alloc_clean(pool_bo_bytes(bytes), "
				     "max_t(u64, align, nvxnu_vm_page_size()), &h)"));
	EXPECT_THAT(definition(gpu(), "void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(gpu(),
			       "void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)"),
		    HasSubstr("bytes = pool_bo_bytes(bytes)"));
	EXPECT_THAT(definition(SourceText("../shim/linux.c"),
			       "void *nvxnu_pool_alloc_clean(unsigned long size, unsigned long "
			       "align, unsigned long long *pa)"),
		    HasSubstr("align >> PAGE_SHIFT"));
	EXPECT_THAT(
		definition(gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)"),
		HasSubstr("i * TGM_FENCE_SLOT"));
	EXPECT_THAT(client(), HasSubstr("IOAddressRange range = {fence_pa_, "
					"static_cast<IOByteCount>(PAGE_SIZE)};"));
}

TEST(GM20BPoolScrubSource, ScrubberFreesOnlyFromItsFence)
{
	const std::string fenced = definition(gpu(), "void scrub_fenced(struct tgm_chan *chan)");
	EXPECT_THAT(fenced, HasSubstr("nvxnu_pool_free_zeroed("));
	EXPECT_THAT(fenced, HasSubstr("tgm_chan_completed(chan)"));
	EXPECT_EQ(Count(gpu(), "nvxnu_pool_free_zeroed("), 2u)
		<< "tgm_pool_free_zeroed's and the scrubber's";
	const std::string submit = definition(gpu(), "static int scrub_submit(u64 pa, u64 bytes)");
	EXPECT_THAT(submit, HasSubstr("tgm_fill_setup(push->cur, 0)"));
	EXPECT_THAT(
		submit,
		HasSubstr(
			"chan_fence(chan)"));  // WFI, FB flush, L2 flush, release, nonstall interrupt
	EXPECT_THAT(submit, HasSubstr("nvxnu_work_schedule_after(tgm_scrub.deadline, SCRUB_NS)"));
	EXPECT_THAT(submit, Not(HasSubstr("nvxnu_idle")));  // no CPU wait for the copy engine
	EXPECT_THAT(definition(gpu(), "static void scrub_deadline(void *arg)"),
		    HasSubstr("nvxnu_panic("));
	EXPECT_THAT(
		definition(
			gpu(),
			"static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"),
		HasSubstr("chan->kernel == CHAN_SCRUB"));
}

TEST(GM20BPoolScrubSource, FreedPoolBosGoToTheGpuFirst)
{
	expectBefore(
		definition(client(), "bool poolZeroLater(void *va, uint64_t pa, uint64_t bytes)"),
		"tgm_pool_scrub(pa, bytes)", "spares()");
}

// The user client reads pushes only through the header gm20b_push_scan_test tests, and profiles only what it read.
TEST(GM20BPushScanSource, KernelUsesTheHeader)
{
	EXPECT_THAT(client(), HasSubstr("#include \"TegraGM20BPushScan.h\""));
	EXPECT_THAT(client(), Not(HasSubstr("void pushMethod(")));
	EXPECT_THAT(client(), Not(HasSubstr("void pushParse(")));
	const std::string scan =
		Between(client(), "void AGXDeviceUserClient::scanPushes(", "\n}\n");
	EXPECT_THAT(scan, HasSubstr("scanPush(c, p, whole, nullptr, 0)"));
	EXPECT_THAT(scan, HasSubstr("prof->cls[i] = cls;"));
	// A stamp only after a push the profile's rule left a class (switch_gpu_prof_stamped), only on a profiled submission.
	EXPECT_THAT(Between(gpu(), "void chan_job_entry(", "\n}\n"),
		    HasSubstr("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	// A job with internal fences is profiled whole: read at its last seqno, against its first's start stamp.
	EXPECT_THAT(Between(gpu(), "static void chan_submission_room(", "\n}\n"),
		    HasSubstr("|| count > TGM_PROF_MAX || profiled))"));
	EXPECT_THAT(Between(gpu(), "int chan_exec_body(", "\n}\n"), HasSubstr("chan->submitted + 1 + fences); /*"));
	EXPECT_THAT(Between(gpu(), "static inline void stats_fences(", "\n}\n"),
		    HasSubstr("prof_collect(chan, first, fs->start.ns, st->end.ns)"));
}

// Direct scanout's fence-only present (NativeMobileDisplay, TEGRA_SCANOUT_COPY_FENCE_ONLY) launches no copy.
TEST(GM20BPresentSource, FenceOnlyPresentHasNoCopy)
{
	EXPECT_THAT(flat(Between(gpu(), "static int present_copy(", "\n}\n")),
		    HasSubstr("if (dst_va) tgm_present_push_copy(push);"));
}

// gpu.poster_targets reads each process's largest colour target, which the stats' push scan records (debug kernels).
// Every submission path takes the scan before its kick and hands its profile to the kick, so raw's SubmitJob jobs
// (57b71739d) are read as its Exec jobs were; the scan's counts are atomic, as SubmitJob runs without the connection lock.
TEST(GM20BPushScanSource, EverySubmissionPathScansItsPushes)
{
	const std::string job =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,"));
	expectBefore(job, "const tgm_prof *profile = scanSubmission(pushes, count, prof, true);", "switch_gpu_chan_exec_events(");
	EXPECT_THAT(job, HasSubstr("&seqno, profile, jobVisibility, releases, keptCount);"));
	const std::string exec =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::exec(uint32_t channel, TegraGM20BAbiPush *input,"));
	expectBefore(exec, "const tgm_prof *profile = scanSubmission(pushes, count, prof, false);",
		     "switch_gpu_chan_exec_events(");
	EXPECT_THAT(exec, HasSubstr("&seqno, profile, jobVisibility, nullptr, 0);"));
	EXPECT_EQ(Count(client(), "scanPushes(pushes, count,"), 1u);
	EXPECT_THAT(flat(definition(client(), "const tgm_prof *AGXDeviceUserClient::scanSubmission(")),
		    HasSubstr("__atomic_fetch_add(&pushSubmits_, 1u, __ATOMIC_RELAXED) % kScanEvery"));
}

// A copying present flushes the L2 once, before the copy reads the frame; its copy writes around the L2, so its fence
// does not flush again. A fence-only present (direct scanout of a frame the GPU drew through the L2) keeps the fence's.
TEST(GM20BPresentSource, OneL2FlushPerPresent)
{
	const std::string copy = flat(Between(gpu(), "static int present_copy(", "\n}\n"));
	expectBefore(flat(Between(gpu(), "void tgm_present_push_copy(", "\n}\n")),
		     "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "tgm_copy_texels(");
	EXPECT_THAT(copy,
		    HasSubstr("if (dst_va) chan_fence_volatile(chan); else chan_fence(chan);"));
	const std::string fence = flat(definition(gpu(), "static u32 chan_fence_job("));
	expectBefore(fence, "NVB06F_FB_FLUSH, 0); if (external != TGM_FENCE_VOLATILE_WRITES) {",
		     "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY");
}

// A present waits for exactly the producers its surfaces' backings name (TegraScanoutCopy alsoWritten: window B's too),
// never for every channel whose address space maps them; window B's own fence maps nothing and copies nothing.
TEST(GM20BPresentSource, PresentWaitsForTheSurfacesProducersOnly)
{
	const std::string copy = flat(Between(gpu(), "static int present_copy(", "\n}\n"));
	EXPECT_THAT(copy, Not(HasSubstr("tgm_live_chans")));
	EXPECT_THAT(copy, Not(HasSubstr("vm_imports")));
	EXPECT_THAT(gpu(), Not(HasSubstr("int tgm_present_copy(")));
	EXPECT_THAT(gpu(), Not(HasSubstr("int tgm_present_copy_read(")));
	EXPECT_THAT(flat(Between(client(), "extern \"C\" int tegra_gm20b_scanout_copy(", "\n}\n")),
		    HasSubstr("switch_gpu_backing_find_pa(copy->alsoWritten & ~0xfffULL)"));
	const std::string fence =
		flat(Between(client(), "extern \"C\" int tegra_gm20b_scanout_fence(", "\n}\n"));
	expectBefore(fence, "switch_gpu_backing_find_pa(pa & ~0xfffULL)", "gPresentSnapshot.add(backing)");
	expectBefore(fence, "gPresentSnapshot.add(backing)", "tgm_present_copy_dependencies(0, 0, -1, 1, 1, 0, 0, 0, "
							      "SwitchGpuScanoutSnapshot::complete, &gPresentSnapshot, "
							      "gPresentSnapshot.tokens, gPresentSnapshot.count)");
	EXPECT_THAT(fence, HasSubstr("if (ret && !presentBusy) gPresentSnapshot.cancel();"));
	EXPECT_THAT(fence, HasSubstr("switch_gpu_backing_release(backing);"));
	EXPECT_THAT(fence, Not(HasSubstr("presentSurface(")));
}

// The scanout read-back (TegraGM20BReadbackABI.h) rides the frame's own present: a submission of its own would hold the
// present channel, which then refuses the display's next offer, and the display copies that frame on the CPU. Its copies
// run after the frame's copy, each after the copies before it, into pitch-linear buffers, and before the fence; a refused
// present gives the read-back back; its completion, in the GPU interrupt pass, takes no lock and touches no cache; and a
// release kernel has none of it.
TEST(GM20BPresentSource, ReadbackRidesTheFramesPresent)
{
	const std::string copy = flat(Between(gpu(), "static int present_copy(", "\n}\n"));
	expectBefore(copy, "if (dst_va) tgm_present_push_copy(push);",
		     "present_push_reads(push, reads, nreads);");
	expectBefore(copy, "present_push_reads(push, reads, nreads);", "chan_fence(chan);");
	EXPECT_THAT(copy, HasSubstr("nreads * TGM_COPY_RECT_DWORDS"));
	const std::string reads = flat(Between(gpu(), "static void present_push_reads(", "\n}\n"));
	EXPECT_THAT(reads, HasSubstr("dst = {r->dst_va, r->dst_pitch, -1};"));  // pitch-linear
	EXPECT_THAT(reads,
		    HasSubstr("tgm_copy_texels(push->cur, &src, &dst, r->width, r->height)"));
	expectBefore(gpu(), "#if TGM_STATS\n/* Each read-back", "static void present_push_reads(");

	const std::string scanout =
		flat(Between(client(), "extern \"C\" int tegra_gm20b_scanout_copy(", "\n}\n"));
	expectBefore(scanout, "#if TGM_STATS tgm_present_read reads",
		     "#else gPresentSnapshot.done = presentDone; ret = watch ? presentWatched() : 1; if (ret "
		     "== 1) ret = tgm_present_copy_dependencies(");
	EXPECT_THAT(scanout, HasSubstr("nreads ? tgm_readback_done : presentDone"));
	EXPECT_THAT(scanout, HasSubstr("if (ret && nreads) tgm_readback_refused();"));

	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const std::string done =
		flat(definition(readback, "void tgm_readback_done(void *display, int status)"));
	EXPECT_THAT(done, HasSubstr("native_legacy_gpu_complete(display, status);"));
	EXPECT_THAT(done, Not(HasSubstr("IOLock")));
	EXPECT_THAT(done, Not(HasSubstr("tgm_cpu_clean_pages")));
	expectBefore(readback, "#include \"TegraGM20BReadback.h\"\n\n#if TGM_STATS\n", "namespace");
	EXPECT_EQ(readback.substr(readback.size() - 7), "#endif\n");
}

// Live (ui.glass_live): every present, armed or not, records what window A will scan for it, so the next armed present
// can read the frame shown before it again, as it is at the end of its time on screen; only an Arm takes the live flag.
TEST(GM20BPresentSource, LiveReadbackRereadsTheFrameShownBefore)
{
	const std::string readback = SourceText("../TegraGM20BReadback.cpp");
	const std::string take = flat(Between(readback, "uint32_t tgm_readback_take(", "\n}\n"));
	expectBefore(take, "const Shown before = gShown;", "int armed = TegraGM20BReadbackArmed;");
	expectBefore(take, "gShown.sequence = copy->sequence;",
		     "int armed = TegraGM20BReadbackArmed;");
	EXPECT_THAT(take, HasSubstr("gShown.read = direct ? tgm_present_read{source,"));
	EXPECT_THAT(take, HasSubstr(": tgm_present_read{target,"));
	expectBefore(take, "reads[1] = before.read;", "reads[1].dst_va = bufferVa(1);");
	EXPECT_THAT(take,
		    HasSubstr("r.live && before.read.src_va && before.read.width == copy->width"));
	const std::string call = flat(Between(readback, "IOReturn tgm_readback_call(", "\n}\n"));
	EXPECT_THAT(
		call,
		HasSubstr("in->reserved > 1 || (in->reserved && in->op != TegraGM20BReadbackArm)"));
	EXPECT_THAT(call, HasSubstr("arm(owner, in->reserved == 1)"));
}

// A tiled (block-linear) source: the copy engine reads it block-linear with the tiled block height, never as pitch, through
// a mapping of the kind its texels were written under; a surface's layout comes from its decision (SurfaceLayout), never
// from how one client bound it.
TEST(GM20BPresentSource, TiledSourceIsCopiedBlockLinear)
{
	const std::string copy = flat(Between(gpu(), "void tgm_present_push_copy(", "\n}\n"));
	EXPECT_THAT(copy, HasSubstr("const u32 src_pitch = tgm_present.copy.src_block_log2 >= 0 ? "
				    "tgm_present.copy.width * 4 : (u32)tgm_present.copy.stride;"));
	EXPECT_THAT(copy, HasSubstr("{tgm_present.copy.src_va, src_pitch, "
				    "tgm_present.copy.src_block_log2}"));
	const std::string scanout =
		flat(Between(client(), "extern \"C\" int tegra_gm20b_scanout_copy(", "\n}\n"));
	EXPECT_THAT(
		scanout,
		HasSubstr("sourceBlock ? static_cast<int32_t>(copy->sourceBlockHeightLog2) : -1;"));
	EXPECT_THAT(scanout,
		    HasSubstr("tgm_readback_take(copy, source, blockLog2, target, reads)"));
	EXPECT_THAT(flat(Between(SourceText("../TegraGM20BReadback.cpp"),
				 "uint32_t tgm_readback_take(", "\n}\n")),
		    Not(HasSubstr("TEGRA_SCANOUT_TILED")));
	const std::string surface = flat(Between(client(), "uint64_t presentSurface(", "\n}\n"));
	EXPECT_THAT(surface, HasSubstr("const uint32_t kind = blockLinear ? TGM_LAYOUT_KIND : 0;"));
	EXPECT_THAT(surface, HasSubstr("s.mem, bytes, static_cast<uint8_t>(kind))"));
	EXPECT_THAT(flat(Between(client(), "IOReturn AGXDeviceUserClient::vmBind(", "\n}\n")),
		    Not(HasSubstr("tiled")));
	EXPECT_THAT(flat(Between(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(",
				 "\n}\n")),
		    HasSubstr("tgm_coherency_layout(coh,"));
	const std::string info = flat(
		Between(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo(", "\n}\n"));
	EXPECT_THAT(info, HasSubstr("? tgm_coherency_layout_fitted(coh, width, height, "
				    "surface.bpp, pitch, writes)"));
	EXPECT_THAT(info, HasSubstr("canonical || (same && tgm_layout_nil_height(&tiled))"));
}

// System sleep (gpu_power.c "system sleep"): every path that touches the GPU outside tgm_gpu.lock goes through the gate
// gm20b_sleep_test runs, and sleep and wake keep the orders the hardware needs.
TEST(GM20BSleepSource, GpuTouchingPathsGoThroughTheGate)
{
	for (const char *waits : {"int tgm_chan_exec(", "int tgm_chan_new("}) {
		const std::string body = flat(definition(gpu(), waits));
		expectBefore(body, "gate_enter();", "gate_leave();");
	}
	for (const char *refused : {"int tgm_present_copy_dependencies(",
				    "int tgm_present_copy_read_dependencies(",
				    "int tgm_pool_scrub(uint64_t pa, uint64_t bytes)",
				    "static void scrub_work(void *arg)"}) {
		const std::string body = flat(definition(gpu(), refused));
		expectBefore(body, "if (!gate_try())", "gate_leave();");
	}
	// the drain's and the wake's fence waits are woken by the scrubber's fences too
	EXPECT_THAT(flat(definition(gpu(), "static int chan_nonstall(struct nvif_event *event, "
					   "void *repv, u32 repc)")),
		    HasSubstr("if (tgm_gate_is_shut(&tgm_sc7_gate)) nvxnu_wake();"));
	// a pass for an interrupt taken before the GPU's power went reads nothing
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("if (__atomic_load_n(&gGpuOff, __ATOMIC_ACQUIRE)) return;"));
}

// Fence waits sleep on their channel's word: a fence interrupt wakes only the waiters of a channel whose fence reached
// the lowest seqno one of them published, a kill or a teardown wakes that channel's, and the interrupt pass's own wake is
// left to the kernel's waits.
TEST(GM20BFenceWakeSource, WaitersWakeByChannel)
{
	const std::string nonstall = flat(definition(
		gpu(), "static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(nonstall,
		    HasSubstr("} else { chan_wake_waiters(chan); present_watch_fenced(chan, 0); }"));
	EXPECT_EQ(Count(nonstall, "nvxnu_wake();"),
		  1u);  // the scrubber's, while the sleep gate is shut
	const std::string wake =
		flat(definition(gpu(), "static void chan_wake_waiters(struct tgm_chan *chan)"));
	EXPECT_THAT(wake, HasSubstr("if (done == chan->woken_done) return;"));
	EXPECT_THAT(wake, HasSubstr("if (switch_gpu_waiter_reached(&chan->wanted, done)) "
				    "chan_wake_all_waiters(chan);"));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_chan_killed(struct nvif_event *event, void "
					   "*repv, u32 repc)")),
		    HasSubstr("chan_wake_all_waiters(chan);"));
	expectBefore(flat(definition(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")),
		     "*pchan = NULL;", "nvxnu_word_wake(&tgm_waiter_words[waiter]);");
	EXPECT_THAT(flat(definition(
			    SourceText("../shim/xnu_glue.cpp"),
			    "void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)")),
		    HasSubstr("nvxnu_wake();"));
	const std::string wait =
		flat(Between(client(), "IOReturn AGXDeviceUserClient::waitFence(", "\n}\n"));
	expectBefore(wait, "seq = tgm_waiter_seq(waiter);", "tgm_chan_want(chan, in->seqno);");
	expectBefore(wait, "tgm_chan_want(chan, in->seqno);",
		     "tgm_chan_completed(chan) - in->seqno");
	EXPECT_THAT(wait, HasSubstr("tgm_waiter_idle(waiter, seq, deadline)"));
	// a seqno passed silently (an internal fence) is announced only by its job's end
	expectBefore(wait, "tgm_chan_announced(chan, in->seqno);",
		     "tgm_waiter_unwoken(waiter, seq, in->seqno);");
	EXPECT_THAT(wait, Not(HasSubstr("tgm_idle(")));
}

// A killed channel is dead alone (client fault isolation): what names it refuses, the connection's memory, binds and other
// channels go on, a new channel can be made in it, and nothing another channel waits for stays on the dead one.
TEST(GM20BKillSource, AKilledChannelLeavesItsConnectionWorking)
{
	const std::string run =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,"));
	EXPECT_THAT(client(), Not(HasSubstr("dead_")));
	EXPECT_THAT(run, Not(HasSubstr("tgm_chan_error(")));
	EXPECT_THAT(flat(definition(gpu(), "int chan_exec_body(struct tgm_chan *chan,")),
		    HasSubstr("if (chan->error) return -EIO;"));
	const std::string order =
		flat(definition(gpu(), "int tgm_chan_order(struct tgm_chan *chan,"));
	EXPECT_THAT(order, HasSubstr("if (READ_ONCE(chan->error)) return -EIO;"));
	EXPECT_THAT(order, HasSubstr("READ_ONCE(other->error)) continue;"));
	const std::string killed = flat(definition(
		gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)"));
	EXPECT_THAT(killed, HasSubstr("__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);"));
	EXPECT_THAT(killed, HasSubstr("chan->kernel || !tgm_all_clients()"));
	EXPECT_LT(killed.find("switch_gpu_timeline_failed("), killed.find("__atomic_store_n(chan->fence_cpu"));
	const std::string freed = flat(definition(gpu(), "static void chan_free(struct tgm_chan *chan)"));
	EXPECT_LT(freed.find("nvif_event_dtor(&chan->kill)"), freed.find("switch_gpu_timeline_close("));
	EXPECT_LT(freed.find("nvif_event_dtor(&chan->nonstall)"), freed.find("switch_gpu_timeline_close("));
	EXPECT_THAT(flat(definition(client(), "bool AGXDeviceUserClient::bindingIdle(")),
		    HasSubstr("!tgm_chan_error(chans_[slot])"));
}

// The raw path's reopen after its channel is killed: the shader heap's BO and VA outlive the channel, so every program
// placed in it (Device::place) keeps its slot and every state that baked its offset stays drawable; the new channel gets
// what lives in a channel's context again, the program region at the heap, the texture and sampler pools and the root
// constant buffer (emitContext, from the pack's relocations) and the 2D state, and the first command buffer's prologue
// invalidates the shader caches before any draw.
TEST(GM20BRawReopenSource, ReopenKeepsTheHeapAndReplaysTheContext)
{
	const std::string raw = SourceText("../../TegraGM20BMetal/raw/runtime/raw_gpu.cpp");
	// raw_channel_recovery.h makeChannel makes the channel, then runs newContext on it
	const std::string reopen = flat(definition(raw, "bool Device::reopen()")) +
				   flat(definition(raw, "bool Device::newContext(uint32_t channel, uint32_t &seq)"));
	EXPECT_THAT(reopen, Not(HasSubstr("placed_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heapPlaced_")));
	EXPECT_THAT(reopen, Not(HasSubstr("pipelines_")));
	EXPECT_THAT(reopen, Not(HasSubstr("heap_")));
	expectBefore(reopen, "makeChannel(k_, kEngines, channel_, fence_", "emitContext(p);");
	expectBefore(reopen, "emitContext(p);", "fx::emitTwodInit(p);");
	// the interlock's ticket buffer zeroed by the copy engine in the new channel, after the context names it and before
	// the 3D engine runs again
	expectBefore(reopen, "emitContext(p);",
		     "uint32_t(tickets_.va >> 32), uint32_t(tickets_.va)}, SUBC_COPY);");
	expectBefore(reopen, "uint32_t(tickets_.va)}, SUBC_COPY);",
		     "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);");
	expectBefore(reopen, "p.m(CE_LAUNCH_DMA, kLaunchPitch, SUBC_COPY);",
		     "p.m(WAIT_FOR_IDLE, 0);");
	const std::string context = flat(definition(raw, "void Device::emitContext(Push &p)"));
	EXPECT_THAT(context, HasSubstr("p.m(LOAD_CB_OFFSET, kRootInterlockTickets);"));
	// the init template relocated to this device's own addresses (raw_pack.cpp relocated, shared with the runtime's capture)
	EXPECT_THAT(context, HasSubstr("const InitAddresses here = {heap_.va, zero_.va, root_.va, "
				       "tic_.va, tsc_.va, kTicCount - 1, kTscCount - 1};"));
	EXPECT_THAT(context, HasSubstr("relocated(im[i], here)"));
	const std::string relocated = flat(
		definition(SourceText("../../TegraGM20BMetal/raw/runtime/raw_pack.cpp"),
			   "uint32_t relocated(const PackInitMethod &m, const InitAddresses &a)"));
	for (const char *reloc :
	     {"case RelocHeapHi:", "case RelocHeapLo:", "case RelocTicHi:", "case RelocTicLo:",
	      "case RelocTscHi:", "case RelocTscLo:", "case RelocRootHi:", "case RelocRootLo:"})
		EXPECT_THAT(relocated, HasSubstr(reloc));
	EXPECT_THAT(context, HasSubstr("sampleWords(p, 1, root_.va);"));
	EXPECT_THAT(
		flat(definition(raw, "void sampleWords(Push &p, uint32_t samples, uint64_t root,")),
		HasSubstr("sampleLayoutMethods(p, l, kRootBytes, root);"));
	EXPECT_THAT(flat(definition(SourceText("../../TegraGM20BMetal/raw/runtime/raw_multisample.h"),
				    "inline void sampleLayoutMethods(")),
		    HasSubstr("p.inc(NVB197_SET_CONSTANT_BUFFER_SELECTOR_A,"));
	const std::string begin = flat(definition(raw, "CommandBuffer *Device::begin()"));
	expectBefore(begin, "!reopen())", "p.m(INVALIDATE_SHADER_CACHES_NO_WFI,");
}

// No spin for GPFIFO or push room: every wait for room sleeps until a fence's interrupt, and the fence reports GET to nvif
// before that interrupt, so the room the GPU freed is there when the waiter looks.
TEST(GM20BRoomSource, RoomIsWaitedForByInterrupt)
{
	EXPECT_EQ(Count(gpu(), "PUSH_WAIT("), 0u);
	EXPECT_EQ(Count(gpu(), "nvif_chan_gpfifo_wait("),
		  3u);
	const std::string wait = flat(definition(
		gpu(),
		"int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)"));
	EXPECT_THAT(wait, HasSubstr("if (!nvxnu_idle(seq, end)"));
	const auto reserve = flat(definition(gpu(), "static inline int chan_wait_room("));
	expectBefore(reserve, "chan_wait_ready(chan, gpfifo_nr, push_nr)",
		     "nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr)");
	expectBefore(flat(definition(gpu(), "static u32 chan_fence_job(struct tgm_chan *chan, int "
					    "external, u32 first, u32 next, int quiet,")),
		     "nvif_chan_gpfifo_post(&chan->chan);", "NON_STALL_INTERRUPT, 0);");
}

TEST(GM20BRoomSource, NativePublicationNeverSleepsWithBackingLocksHeld)
{
	const auto exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "switch_gpu_chan_submit_enter(chan);", "switch_gpu_backing_begin_checked(");
	expectBefore(exec, "switch_gpu_backing_abort(&transaction);",
		"switch_gpu_chan_wait_submission_room(");
	expectBefore(exec, "switch_gpu_backing_commit(&transaction);", "switch_gpu_chan_submit_leave();");
	const auto emit = flat(definition(gpu(), "int chan_exec_body("));
	EXPECT_THAT(emit, HasSubstr("if (!may_wait && !chan_room(chan, entries, words)) return -EAGAIN;"));
	expectBefore(emit, "chan->chan.defer_kick = true;", "nvif_chan_gpfifo_wait(&chan->chan, entries, words)");
	EXPECT_THAT(emit, HasSubstr("chan->chan.gpfifo.cur = ring_cur;"));
}

// SubmitJob holds no connection lock and orders its shared uses' backing locks exactly as exec does.
TEST(GM20BRoomSource, SubmitJobNeverSleepsWithBackingLocksHeld)
{
	const auto job = flat(definition(client(), "IOReturn AGXDeviceUserClient::runJobUses("));
	EXPECT_THAT(job, Not(HasSubstr("IOLockLock(lock_)")));
	expectBefore(job, "switch_gpu_chan_submit_enter(chan);", "switch_gpu_backing_begin_checked(");
	expectBefore(job, "switch_gpu_backing_abort(&transaction);", "switch_gpu_chan_wait_submission_room(");
	expectBefore(job, "switch_gpu_backing_commit(&transaction);", "switch_gpu_chan_submit_leave();");
	expectBefore(job, "switch_gpu_set_note_job(set, slot, &completion);", "switch_gpu_chan_submit_leave();");
	const auto submit = flat(definition(client(), "IOReturn AGXDeviceUserClient::submitJob("));
	expectBefore(submit, "submitGate_.enterFast();", "runJob(record, job, out)");
	expectBefore(submit, "runJob(record, job, out)", "submitGate_.leaveFast(lock_);");
}

// The HW Test's gpu.path.submit_job reads these per process: a job SubmitJob ran, whether it named its set at the current
// generation, a refusal for an ended generation, and a job the older SubmitAddress ran.
TEST(GM20BRoomSource, SubmissionMethodsCountTheirPaths)
{
	const auto run = flat(definition(client(), "IOReturn AGXDeviceUserClient::runJob("));
	expectBefore(run, "switch_gpu_path_taken(SWITCH_GPU_PATH_STALE_SET, 1);", "return kIOReturnNotReady;");
	const auto job = flat(definition(client(), "IOReturn AGXDeviceUserClient::runJobUses("));
	expectBefore(job, "switch_gpu_path_taken(SWITCH_GPU_PATH_SUBMIT_JOB, 1);", "return kIOReturnSuccess;");
	expectBefore(job, "switch_gpu_path_taken(SWITCH_GPU_PATH_SET_JOB, set ? 1 : 0);", "return kIOReturnSuccess;");
	expectBefore(job, "switch_gpu_path_taken(SWITCH_GPU_PATH_SHARED_USE, head.shared_count);", "return kIOReturnSuccess;");
	const auto address = flat(definition(client(), "IOReturn AGXDeviceUserClient::submitAddress("));
	expectBefore(address, "result = exec(submit->channel,",
		     "switch_gpu_path_taken(SWITCH_GPU_PATH_ADDRESS_JOB, result ? 0 : 1);");
	const std::string paths = SourceText("../SwitchGpuPathStats.cpp");
	for (const char *name : {"\"submit_jobs\"", "\"set_jobs\"", "\"stale_sets\"", "\"address_jobs\"", "\"shared_uses\""})
		EXPECT_THAT(paths, HasSubstr(name));
}

// A SubmitJob command buffer of several jobs holds its shared uses' CPU-lock admission from Begin to End, as SubmitAddress
// did: a Continue job takes the admission's guards instead of refreshing its surfaces, and a refused job ends it.
TEST(GM20BRoomSource, SubmitJobHoldsACommandBuffersAdmission)
{
	const auto job = flat(definition(client(), "IOReturn AGXDeviceUserClient::runJobUses("));
	EXPECT_THAT(job, HasSubstr("switch_gpu_claim_flags(admissionBegin || admissionContinue)"));
	expectBefore(job, "if (admissionContinue && !admission.covers(job.uses, useTotal))", "refresh:");
	expectBefore(job, "error = admission.begin(job.uses, useTotal, job.guards, guardCount, job.leases, leaseCount);",
		     "switch_gpu_chan_submit_enter(chan);");
	expectBefore(job, "switch_gpu_chan_submit_leave();", "admission.end();");
	const auto submit = flat(definition(client(), "IOReturn AGXDeviceUserClient::submitJob("));
	expectBefore(submit, "(record.job->flags & TegraGM20BJobAdmissionCancel)", "runJob(record, job, out)");
}

// A fence wait woken at its seqno decides from the fence page alone; the connection lock is for publishing, errors and
// timeouts.
TEST(GM20BRoomSource, AWokenFenceWaitTakesNoConnectionLock)
{
	const auto wait = flat(definition(client(), "IOReturn AGXDeviceUserClient::waitFence("));
	expectBefore(wait, "switch_gpu_fence_passed(record, in->seqno, waitedOn)", "IOLockLock(lock_);");
	expectBefore(wait, "if (ret != kIOReturnSuccess) { IOLockLock(lock_);", "tgm_chan_want(chan, in->seqno);");
}

// The waiter a submitter sleeps in after -EAGAIN waits for exactly the room chan_exec then checks, computed in one place:
// a waiter that returned with less left the retry loops (TegraGM20BCopy.cpp submit, the user client's exec) spinning.
TEST(GM20BRoomSource, WaiterAndSubmissionReserveOneRoom)
{
	EXPECT_EQ(Count(gpu(), "5 * (TGM_ORDER_MAX + dependency_count"), 1u);
	const auto room = flat(definition(gpu(), "static void chan_submission_room("));
	EXPECT_THAT(room, HasSubstr("5 * (TGM_ORDER_MAX + dependency_count + 1)"));
	// SubmitJob's sibling acquires take a dependency's room; its retry asks the waiter for both together.
	expectBefore(flat(definition(gpu(), "int chan_exec_body(")),
		     "chan_submission_room(chan, pushes, count, dependency_count + sibling_count + word_wait_count, "
		     "fences, &prof, &entries, &words);",
		     "if (!may_wait && !chan_room(chan, entries, words)) return -EAGAIN;");
	expectBefore(flat(definition(gpu(), "int switch_gpu_chan_wait_submission_room(")),
		     "chan_submission_room(chan, pushes, count, dependency_count, fences, &prof, &entries, &words);",
		     "ret = tgm_chan_wait_ready(chan, entries, words);");
}

// The L2 invalidate that makes the CPU's writes visible is ordered on the GPU, in the submission's own stream, ahead of
// a first push that is not prefetched; the CPU does not wait for it. A channel behind its space's GPU-ordered maps
// invalidates its TLB after the L2 (the walk reads page tables through it) in the same entry.
TEST(GM20BL2Source, InvalidateOnTheGpu)
{
	EXPECT_EQ(Count(gpu(), "FLUSH_L2_SYSTEM_INVALIDATE"), 0u);
	const std::string exec = flat(Between(gpu(), "int chan_exec_body(", "\n}\n"));
	expectBefore(exec, "tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;",
		     "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);");
	// a pending bind's acquire comes before the TLB invalidate that makes its new PTEs seen
	expectBefore(
		exec,
		"bind = !ret && !chan->kernel && bind_pending(chan->vm, &bind_target, &bind_va);",
		"tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;");
	expectBefore(
		exec, "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);",
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);");
	expectBefore(
		exec,
		"NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE); } PUSH_KICK(&chan->chan.push);",
		"job.unprefetched = (l2_on() && host_written) || tlb;");
	EXPECT_THAT(flat(Between(gpu(), "void chan_job_entry(", "\n}\n")),
		    HasSubstr("pushes[i].no_prefetch || (job->unprefetched && !i));"));
}

// A CPU IOSurfaceLock waits for the submissions that may write the surface: Exec names each one once it has its seqno.
TEST(GM20BCoherencySource, ExecNamesTheWriterOnceTheSubmissionHasItsSeqno)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "error = switch_gpu_chan_exec_events(",
		     "surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);");
	expectBefore(exec, "error = switch_gpu_chan_exec_events(", "switch_gpu_backing_commit(&transaction);");
	const std::string after = flat(definition(SourceText("../TegraGM20BExecSurfaces.cpp"),
						  "void ExecSurfaces::after("));
	EXPECT_THAT(after, HasSubstr("tgm_coherency_wrote(refs_[i].coh, always, uid, seqno);"));
	// only a listed write is a writer another channel waits for on the GPU (gm20b_fences_test)
	EXPECT_THAT(after, HasSubstr("tgm_coherency_wrote_listed(r->coh, uid, seqno);"));
}

// Method 17's acquires go into the submission's own stream: queued (order_add, under tgm_live_lock) before tgm_chan_exec makes
// it, every exit of Exec past the channel check having taken the list; the decision is TegraGM20BFences.h's, which
// gm20b_fences_test runs.
TEST(GM20BFencesSource, ExecQueuesItsWritersAcquiresBeforeItSubmits)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "if (followed < 0) return ioreturn(followed);",
		     "error = switch_gpu_chan_exec_events(");
	expectBefore(exec, "switch_gpu_backing_dependencies(", "error = switch_gpu_chan_exec_events(");
	EXPECT_THAT(exec, HasSubstr("ownershipBusy ? kIOReturnCannotLock : ioreturn(error)"));
	// Named uses carry no CPU exclusion unless an admission ordered them; a legacy job claims only listed imports.
	EXPECT_THAT(exec, HasSubstr("const uint32_t claim = switch_gpu_claim_flags(admissionBegin "
				    "|| admissionContinue);"));
	EXPECT_THAT(exec, HasSubstr("addUse(bo, uses[i].access, claim, offset, bytes)"));
	EXPECT_THAT(exec, HasSubstr("addUse(bo, SWITCH_GPU_BACKING_READ, claim)"));
	EXPECT_THAT(exec, HasSubstr("switch_gpu_legacy_uses("));
	for (const char *signature :
	     {"int tgm_chan_follow(struct tgm_chan *chan,", "int tgm_chan_follow_importers("}) {
		const std::string follow = flat(definition(gpu(), signature));
		expectBefore(follow, "mutex_lock(&tgm_live_lock);", "tgm_fence_wanted(");
		expectBefore(follow, "tgm_fence_wanted(", "ret = order_add(chan,");
		expectBefore(follow, "ret = order_add(chan,", "mutex_unlock(&tgm_live_lock);");
	}
	const auto capabilities = flat(definition(client(), "uint32_t switch_gpu_info_flags(void)"));
	for (const char *capability : {"TegraGM20BInfoExecSurfaces", "TegraGM20BInfoSubmitAddress",
				      "TegraGM20BInfoCpuAccess"})
		EXPECT_THAT(capabilities, HasSubstr(capability));
	EXPECT_THAT(flat(definition(client(), "IOReturn AGXDeviceUserClient::getInfo(")),
		    HasSubstr("out->flags = switch_gpu_info_flags();"));
	EXPECT_THAT(client(), Not(HasSubstr("kInfoAwaitingUserspace")));
}

// The flags exec hands chan_exec are SwitchGpuJobVisibility.h's (switch_gpu_job_visibility_test), with a write into an
// IOSurface over its creator's memory, which IOSurface's own field names at the import, as one that ends External.
TEST(GM20BVisibilitySource, AJobWritingCreatorMemoryIsExternal)
{
	const std::string imported = flat(definition(client(), "IOReturn AGXDeviceUserClient::boImport("));
	expectBefore(imported, "bo->surface = switch_gpu_surface_hold(",
		     "bo->unlockedReads = switch_gpu_surface_client_memory(bo->surface) != 0;");
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "if (access & SWITCH_GPU_BACKING_WRITE) {", "unlockedWrites |= bo->unlockedReads;");
	EXPECT_THAT(exec, HasSubstr("switch_gpu_job_visibility(visibility, events && events->signals(), "
				    "unlockedWrites || scanoutWrites, hostWritten || transaction.host_written)"));
	const std::string surfaces = flat(SourceText("../TegraGM20BSurfaceClient.cpp"));
	EXPECT_THAT(flat(definition(surfaces, "int switch_gpu_surface_client_memory(const OSObject *surface)")),
		    HasSubstr("at<uint64_t>(surface, OriginalIOSurface::clientMemory) != 0"));
}

// FRAMECPU C6.1: a job writing a backing the display scans in place (a direct scanout slot) ends External in both submit
// paths, so its completion means its writes are in DRAM and the display may flip with no present job.
TEST(GM20BVisibilitySource, AJobWritingAScannedBackingIsExternal)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "if (access & SWITCH_GPU_BACKING_WRITE) {",
		     "scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;");
	const std::string job = flat(SourceText("../TegraGM20BUserClientJobSubmit.cpp"));
	expectBefore(job, "if (access & TegraGM20BSurfaceWrite) {",
		     "scanoutWrites |= switch_gpu_backing_scanned(bo->backing) != 0;");
	EXPECT_THAT(job, HasSubstr("switch_gpu_job_visibility(submit, events->signals(), unlockedWrites || "
				   "scanoutWrites, hostWritten || transaction.host_written)"));
	const std::string backing = flat(SourceText("../SwitchGpuBacking.cpp"));
	EXPECT_THAT(flat(definition(backing, "extern \"C\" int switch_gpu_backing_scanned(")),
		    HasSubstr("__atomic_load_n(&backing->scanout_holds, __ATOMIC_ACQUIRE) != 0"));
}

// A SurfaceWrites a client could not deliver never leaves a GPU write unmarked: an Exec flagged
// TegraGM20BPushImportsWritten marks every writable import of its connection (NVK's flag is gm20b_nvk_source_test's;
// the raw path's imports, raw_kernel_memory_test's ASurfaceWritesTheKernelRefusedMarksEveryLaterImportWritten).
TEST(GM20BCoherencySource, AFailedSurfaceWritesFallsBackToMarkingEveryImport)
{
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "importsWritten_ = true;",
		     "surfaces_.after(listed, importsWritten_, tgm_chan_uid(chan), seqno);");
}

// Only a connection's maps skip the CPU's L2 and TLB maintenance; every removal keeps it (TegraGM20BTlb.h).
TEST(GM20BTlbSource, MapsOrderedOnTheGpuRemovalsSynchronous)
{
	EXPECT_THAT(client(), HasSubstr("switch_gpu_vm_map_volatile(vm_, op.va"));
	EXPECT_EQ(Count(client(), "tgm_vm_map("), 0u);
	EXPECT_EQ(Count(gpu(), "nvxnu_gpu_ordered_begin();"), 3u);
	const std::string map = flat(definition(gpu(), "int tgm_vm_map_raw("));
	expectBefore(map, "nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put(");
	const std::string mixed = flat(definition(gpu(), "int switch_gpu_vm_map_volatile("));
	expectBefore(mixed, "nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_unmap(");
	EXPECT_THAT(mixed, HasSubstr("entry->va, entry->bytes, entry->shift"));
	const std::string unmap = flat(definition(gpu(), "void switch_gpu_vm_unmap("));
	EXPECT_THAT(unmap, Not(HasSubstr("gpu_ordered")));
	EXPECT_THAT(unmap, HasSubstr("entry->va, entry->bytes, entry->shift"));
	EXPECT_THAT(flat(definition(gpu(), "void tgm_vm_del(")),
		    HasSubstr("mmu_invalidate_all();"));
	// a cut changes no PTE; what it leaves pending is installed GPU-ordered, its failures put outside that scope
	EXPECT_THAT(flat(definition(gpu(), "int switch_gpu_vm_split(")),
		    Not(HasSubstr("nvif_vmm_raw")));
	const std::string install = flat(definition(gpu(), "int switch_gpu_vm_install("));
	expectBefore(install, "nvxnu_gpu_ordered_end(ordered_scope);", "nvif_vmm_raw_put(");
}

// Rail-gating (TegraGM20BRail.h): every other tgm_gpu.lock taker goes through tgm_gpu_lock(), which wakes a rail-gated GPU in
// the caller under the lock it took; the rail-up opens the gate before it clears `railed`, so a closer that has seen the
// GPU up never has its close undone (gm20b_rail_test's model).
TEST(GM20BRailSource, GpuLockWakesARailGatedGpu)
{
	EXPECT_EQ(Count(gpu(), "mutex_lock(&tgm_gpu.lock);"), 6u)
		<< "gpu_lock, tgm_timestamp_ns, tgm_sleep, tgm_rail_down, tgm_rail_up, "
		   "tgm_rail_sc7_hold";
	EXPECT_THAT(flat(definition(gpu(), "noinline void tgm_gpu_lock(void)")),
		    HasSubstr("if (unlikely(tgm_gpu.railed)) "
			      "tgm_rail_up_locked(__builtin_return_address(0));"));
	expectBefore(flat(definition(gpu(), "void tgm_rail_up_locked(const void *site)")),
		     "gate_open();", "__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);");
	// tgm_rail_up_locked takes these under the caller's tgm_gpu.lock: no tgm_gpu_lock() caller may hold one
	const std::string &text = gpu();
	for (size_t at = text.find("\ttgm_gpu_lock();"); at != std::string::npos;
	     at = text.find("\ttgm_gpu_lock();", at + 1)) {
		const size_t body = text.rfind("\n{\n", at);
		const std::string before = text.substr(body, at - body);
		for (const char *lock : {"(&tgm_live_lock)", "(tgm_idle.clk)", "(&tgm_gate_lock)"})
			EXPECT_EQ(Count(before, std::string("mutex_lock") + lock),
				  Count(before, std::string("mutex_unlock") + lock))
				<< lock << " held at the gpu_lock() at offset " << at;
	}
	// GPU time never backwards (gm20b_rail_test's model): the floor read before every power-down, PTIMER reseeded to it
	// after nvkm_timer_init, every time handed out clamped to it
	expectBefore(flat(definition(gpu(), "static void sleep_nvkm(void)")),
		     "tgm_stamp_last = tgm_stamp_monotonic(tgm_stamp_last, "
		     "nvkm_timer_read(device->timer));",
		     "nvkm_device_fini(device, NVKM_SUSPEND)");
	expectBefore(flat(definition(gpu(), "static void wake_m1(void)")),
		     "ret = nvkm_subdev_init(&device->timer->subdev);", "stamp_reseed(device);");
	// a power-up's clock anchor (TegraGM20BClockABI.h) after its PTIMER is set, at boot and at every wake, before a channel
	// runs; a fence page handed out holds them all once zeroed
	expectBefore(flat(definition(gpu(), "static int m1(char *line, unsigned int len)")),
		     "ret = nvkm_subdev_init(&device->timer->subdev);",
		     "tgm_clock_anchor_publish(device);");
	expectBefore(
		flat(definition(gpu(), "static void stamp_reseed(struct nvkm_device *device)")),
		"nvkm_timer_alarm_trigger(device->timer);", "tgm_clock_anchor_publish(device);");
	expectBefore(flat(definition(gpu(), "static void wake_m1(void)")), "stamp_reseed(device);",
		     "nvkm_subdev_init(&device->clk->subdev);");
	expectBefore(
		flat(definition(gpu(),
				"int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")),
		"memset((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT, 0, PAGE_SIZE);",
		"tgm_clock_anchors_fill(i);");
	EXPECT_THAT(flat(definition(gpu(), "uint64_t tgm_timestamp_ns(void)")),
		    HasSubstr("tgm_stamp_last = t = tgm_stamp_monotonic(tgm_stamp_last, t);"));
	const std::string down = flat(definition(gpu(), "int tgm_rail_down(void)"));
	expectBefore(down, "sleep_gate();", "mutex_lock(&tgm_gpu.lock);");
	expectBefore(down, "__atomic_store_n(&tgm_gpu.railed, 1, __ATOMIC_SEQ_CST);",
		     "nvxnu_wakeup(&tgm_sc7_gate.shut);");
}

TEST(GM20BSleepSource, SleepAndWakeOrder)
{
	const std::string body = flat(definition(gpu(), "void tgm_sleep(void)"));
	const std::string sleep = body.substr(body.find("case TGM_SLEEP_FULL:"));
	expectBefore(sleep, "sleep_gate();", "sleep_fences();");
	expectBefore(sleep, "sleep_fences();", "mutex_lock(&tgm_gpu.lock);");
	expectBefore(sleep, "sleep_idle_off();", "sleep_nvkm();");
	expectBefore(sleep, "sleep_nvkm();", "sleep_power_down();");
	const std::string nvkm = flat(definition(gpu(), "static void sleep_nvkm(void)"));
	expectBefore(nvkm, "nvif_client_suspend(&tgm_gpu.client, false)", "nvkm_ltc_flush(");
	// nvgpu_mm_suspend: the L2's comptag lines written back (FLUSH_L2_CLEAN_COMPTAGS) before the data flush
	expectBefore(nvkm, "nvkm_wr32(device, 0x07000c, 0x00000001);", "nvkm_ltc_flush(");
	expectBefore(nvkm, "nvkm_ltc_flush(", "nvkm_device_fini(device, NVKM_SUSPEND)");
	expectBefore(flat(definition(gpu(), "static void sleep_power_down(void)")),
		     "nvxnu_irq_suspend();", "regulator_disable(");
	// the boot's order: the GPCPLL relock (tgm_clk_raise) before ACR, the PMU and GR (tgm_m2_device_init); interrupts on before
	// nvkm's first init that may wait for one
	const std::string woken = flat(definition(gpu(), "void tgm_wake(void)"));
	const std::string wake = woken.substr(woken.find("case TGM_SLEEP_FULL:"));
	expectBefore(wake, "wake_power_up();", "wake_m1();");
	expectBefore(wake, "wake_m1();", "wake_clock();");
	expectBefore(wake, "wake_clock();", "wake_device();");
	expectBefore(wake, "wake_device();", "wake_nvkm();");
	expectBefore(wake, "wake_nvkm();", "wake_fence();");
	expectBefore(wake, "wake_fence();", "wake_open();");
	expectBefore(flat(definition(gpu(), "static void wake_power_up(void)")), "power_up(",
		     "nvxnu_irq_resume();");
	EXPECT_THAT(flat(definition(gpu(), "static void wake_clock(void)")),
		    HasSubstr("clk_raise(line, sizeof(line))"));
	EXPECT_THAT(flat(definition(gpu(), "static void wake_device(void)")),
		    HasSubstr("m2_device_init(device, &at)"));
}


// At M2 the FIFO has no user: nvkm makes its runqueues and runlists and registers its interrupt handler at the first
// channel (nvkm_fifo_oneinit), from its function table. What recover.c and sched.c take over goes into that table.
TEST(GM20BSchedSource, HooksGoWhereTheFifosOneinitReadsThem)
{
	const std::string recover = flat(definition(
		SourceText("../recover.c"), "void tgm_recover_init(struct nvkm_device *device)"));
	EXPECT_THAT(recover, HasSubstr("func->intr = fifo_intr;"));
	EXPECT_THAT(recover, HasSubstr("func->runq = &ctxsw.runq;"));
	EXPECT_THAT(recover, HasSubstr("func->chan.func = &ctxsw.chan;"));
	EXPECT_THAT(recover, HasSubstr("func->intr_ctxsw_timeout = ctxsw_tick;"));
	const std::string sched = SourceText("../sched.c");
	EXPECT_THAT(flat(definition(sched, "void tgm_sched_init(struct nvkm_device *device)")),
		    HasSubstr("tgm_fifo_func(fifo)->runl_ctor = sched_runl_ctor;"));
	EXPECT_THAT(flat(definition(sched, "static int sched_runl_ctor(struct nvkm_fifo *fifo)")),
		    HasSubstr("interleave(fifo->engine.subdev.device, runl);"));
}

// A submission that writes a block-linear IOSurface under a CPU write lock keeps the lock's rows before it runs, with the
// connection's lock dropped and before the active-submit gate's channel entry (the copy engine's wait sleeps).
// The push profile's stamps (debug kernels): one rule (switch_gpu_prof_stamped) decides the room a profiled job takes, the
// pushes that stamp and the classes read back, so the compositor stamps only where its job already waits for idle; and
// every window says per owner what the stamps cost.
TEST(GM20BPushScanSource, OneRuleDecidesAProfiledJobsStamps)
{
	EXPECT_THAT(flat(definition(gpu(), "static void chan_submission_room(")),
		    HasSubstr("stamps += switch_gpu_prof_stamped(chan->critical, &pushes[i], i, count, (*prof)->cls[i]);"));
	EXPECT_THAT(flat(definition(gpu(), "static inline __attribute__((always_inline)) void chan_job_entry(")),
		    HasSubstr("if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {"));
	const std::string body = flat(gpu());
	expectBefore(body, "const int stamped = switch_gpu_prof_stamped(chan->critical, &pushes[i], i, count, prof->cls[i]);",
		     "last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);");
	EXPECT_THAT(body, HasSubstr("__atomic_fetch_add(&chan->prof_span, end - start, __ATOMIC_RELAXED);"));
	EXPECT_THAT(body, HasSubstr("nvxnu_stats(\"tegra-gm20b: gpuprof cost %s:%s\", o[i].name, line);"));
}

// gpu.path.resource_ready: a GPU write under a CPU write lock keeps the lock's rows for the merge only when its
// submission asks before its job is kicked (TegraGM20BViews.h tgm_views_job_writes). Both submission paths ask, once
// their uses are resolved (Exec outside the connection lock); nothing else calls the per-surface hook, so no path can
// skip the list, as SubmitJob did once raw moved onto it (57b71739d).
TEST(GM20BViewsSource, AGpuWriteUnderACpuWriteLockIsMergedNotHeldBack)
{
	const std::string job =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::runJobUses(const SwitchGpuJobRecord &record,"));
	expectBefore(job, "job.written[writtenCount++] = bo->coh;", "tgm_views_job_writes(job.written, writtenCount);");
	expectBefore(job, "tgm_views_job_writes(job.written, writtenCount);", "switch_gpu_chan_submit_enter(chan);");
	expectBefore(job, "tgm_views_job_writes(job.written, writtenCount);", "switch_gpu_chan_exec_events(");
	const std::string exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	expectBefore(exec, "job.written[writtenCount++] = bo->coh;", "tgm_views_job_writes(job.written, writtenCount);");
	expectBefore(exec, "IOLockUnlock(lock_);", "tgm_views_job_writes(job.written, writtenCount);");
	expectBefore(exec, "tgm_views_job_writes(job.written, writtenCount);", "switch_gpu_chan_submit_enter(chan);");
	EXPECT_EQ(Count(client(), "tgm_views_gpu_writes("), 0u);
}

// The PBDMA's ACQUIRE interrupt is gpu_acquire.c's: the timeout goes off in the PBDMA before the bit is cleared (so the
// same acquire cannot raise it again), and nvkm's handler, which would kill the waiter, never sees it.
TEST(GM20BAcquireSource, TheAcquireInterruptIsTakenBeforeNvkmsHandler)
{
	const std::string runq = flat(definition(SourceText("../recover.c"),
		"static bool runq_intr(struct nvkm_runq *runq, struct nvkm_runl *runl)"));
	expectBefore(runq, "tgm_acquire_intr(device, runq->id)",
		     "nvkm_wr32(device, PBDMA_INTR_0(runq->id), TGM_PBDMA_INTR_ACQUIRE)");
	expectBefore(runq, "TGM_PBDMA_INTR_ACQUIRE)", "ctxsw.runq_nvkm->intr(runq, runl)");
	const std::string intr = flat(definition(gpu(), "void tgm_acquire_intr(struct nvkm_device *device, int runq)"));
	expectBefore(intr, "word & ~TGM_ACQUIRE_TIMEOUT_EN", "nvxnu_work_schedule(acq.work)");
	EXPECT_THAT(intr, HasSubstr("nvkm_mask(device, PBDMA_INTR_EN_0(runq), TGM_PBDMA_INTR_ACQUIRE, 0)"));
	EXPECT_THAT(intr, Not(HasSubstr("mutex_lock")));
	EXPECT_THAT(flat(definition(SourceText("../recover.c"), "void tgm_recover_suspend(void)")),
		    HasSubstr("tgm_acquire_suspend();"));
}

// Every channel's RAMFC carries the timeout before its first submission (the subchannel binds), and a channel whose
// timeout an interrupt turned off gets it back only while it is idle and off the PBDMA, as a level change's timeslice.
TEST(GM20BAcquireSource, EveryChannelIsArmedBeforeItsFirstKickAndReArmedWhenIdle)
{
	const auto create = flat(definition(gpu(), "static int chan_new("));
	expectBefore(create, "chan->chid = args.chan.chid;", "tgm_acquire_arm(chan->chid);");
	expectBefore(create, "tgm_acquire_arm(chan->chid);", "PUSH_KICK(push);");
	const auto due = flat(definition(gpu(), "void tgm_chan_ramfc_due(struct tgm_chan *chan)"));
	expectBefore(due, "tgm_sched_loaded(tgm_gpu.device, chan->chid)", "tgm_acquire_arm(chan->chid);");
	EXPECT_EQ(Count(gpu(), "tgm_chan_ramfc_due(chan);"), 2u);
}

// A late wait and an acquire timeout blame by what the candidates' RAMFC says they wait on, and a check that finds a
// channel parked past its timeout with no interrupt says so.
TEST(GM20BAcquireSource, TheHangRuleReadsWhatEachCandidateWaitsOn)
{
	const auto recover = flat(definition(gpu(),
		"enum tgm_hang tgm_hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns)"));
	EXPECT_THAT(recover, HasSubstr("candidate_wait(&waits, chan, &source)"));
	EXPECT_THAT(recover, HasSubstr("kind = candidate_wait(&waits, chans[i], &source);"));
	EXPECT_THAT(recover, HasSubstr("c[i].victim = kind == TGM_WAITS_VICTIM || kind == TGM_WAITS_UNREAD;"));
	EXPECT_THAT(recover, HasSubstr("tgm_acquire_unfired(chans[i], now);"));
	EXPECT_THAT(recover, Not(HasSubstr("chan->waited.cpu")));
}

// Nouveau puts the SW engine on the runlist with id 15 and no status register: PFIFO_ENG_STATUS(15) (0x0026b8)
// PRI-times out, and the GPU's bus error panics. Every per-engine status read is behind a context or a GR check.
TEST(GM20BSchedSource, EngineStatusReadsSkipTheSoftwareEngine)
{
	for (const char *file : {"../sched.c", "../recover.c", "../recover_engine.c"}) {
		const std::string source = SourceText(file);
		size_t reads = 0;
		for (size_t at = source.find("PFIFO_ENG_STATUS(engn->id)"); at != std::string::npos;
		     at = source.find("PFIFO_ENG_STATUS(engn->id)", at + 1)) {
			reads++;
			// nvkm's engine functions with a context decode, which the SW engine's (gf100_engn_sw) lacks
			const size_t decode = source.rfind(
				"static int engn_cxid(struct nvkm_engn *engn, bool *cgid)", at);
			if (decode != std::string::npos && decode > source.rfind("\n}\n", at))
				continue;
			const size_t loop = source.rfind("nvkm_runl_foreach_engn", at);
			ASSERT_NE(loop, std::string::npos) << file;
			const std::string guard = source.substr(loop, at - loop);
			EXPECT_TRUE(guard.find("engn->func->cxid") != std::string::npos ||
				    guard.find("NVKM_ENGINE_GR") != std::string::npos)
				<< file << ": " << guard;
		}
		EXPECT_GT(reads, 0u) << file;
	}
}

// With interrupts off XNU formats an os_log line only through the kernel printf into the message buffer, which has no
// privacy annotations: "%{public}s" prints "{public}s" and drops nvkm's report (the GR and MMU fault lines).
TEST(GM20BShimSource, InterruptPassReportsUsePlainFormats)
{
	const std::string report = flat(definition(SourceText("../shim/xnu_glue.cpp"),
						   "static void nvxnu_report(const char *line)"));
	EXPECT_THAT(report, HasSubstr("os_log_error(OS_LOG_DEFAULT, \"%s\", line)"));
	EXPECT_THAT(report, Not(HasSubstr("%{")));
}

// A channel made in a slot continues its predecessor's fence sequence, so every cursor the interrupt pass compares with
// the completed seqno starts at that fence. From 0, the idle clock would count the predecessor's whole sequence retired,
// its in-flight count (kicked - retired) would underflow, and the GPU would never drop its clock or rail-gate.
TEST(GM20BChannelSource, SeqnoCursorsStartAtTheSlotsFence)
{
	const std::string create = flat(definition(gpu(), "static int chan_new("));
	EXPECT_THAT(create, HasSubstr("chan->submitted = READ_ONCE(*fence_cpu);"));
	for (const char *cursor :
	     {"stat_done", "woken_done", "gov_done", "progress_done", "seen_done"})
		EXPECT_THAT(create,
			    HasSubstr(std::string("chan->") + cursor + " = chan->submitted;"))
			<< cursor;
}

// nvkm_chan_error preempts from the interrupt pass with interrupts off under the channel's spinlock: the wrapped preempt
// decides by tgm_preempt_how and waits for a preempt in flight only in thread context.
TEST(GM20BSchedSource, ThePreemptWaitsOnlyWithInterruptsOn)
{
	const std::string preempt =
		flat(definition(SourceText("../sched.c"),
				"static void sched_chan_preempt(struct nvkm_chan *chan)"));
	EXPECT_THAT(preempt, HasSubstr("switch (tgm_preempt_how(irqs_disabled(), pending)) {"));
	expectBefore(preempt, "case TGM_PREEMPT_RECOVER: nvkm_runl_rc_cgrp(chan->cgrp); return;",
		     "case TGM_PREEMPT_AFTER_WAIT:");
	expectBefore(preempt, "case TGM_PREEMPT_AFTER_WAIT:", "nvkm_wait_msec(");
	expectBefore(preempt, "nvkm_wait_msec(", "case TGM_PREEMPT_NOW:");
	EXPECT_EQ(Count(preempt, "nvkm_wait_msec("), 1u);
}

// Linux's work items, as kits/SwitchLinux's calls (whose host test holds that a call never runs twice at once): nvkm's
// shim allocates them there, a flush runs one still queued, and irqs_disabled() answers.
TEST(GM20BShimSource, WorkItemsKeepLinuxSemantics)
{
	const std::string nvxnu = flat(SourceText("../shim/include/nvxnu.h"));
	EXPECT_THAT(nvxnu, HasSubstr("return switch_linux_call_alloc(fn, arg);"));
	EXPECT_THAT(nvxnu, HasSubstr("return switch_linux_irqs_disabled() ? 0 : 1;"));
	const std::string os = flat(SourceText("../shim/include/nvif/os.h"));
	EXPECT_THAT(
		os,
		HasSubstr("static inline bool flush_work(struct work_struct *w) { return "
			  "switch_linux_call_flush(w->thread_call, nvxnu_work_trampoline, w); }"));
	EXPECT_THAT(os, HasSubstr("#define irqs_disabled() (!nvxnu_irqs_enabled())"));
}

// Rail-down stops what reaches the GPU from callouts as system sleep does: the deadline's holds released while the GPU
// is up and its callouts stopped before the fences drain, back at the rail-up; a context-switch tick's work stopped once
// interrupts are off.
TEST(GM20BRailSource, RailDownStopsTheGpuCallouts)
{
	const std::string down = flat(definition(gpu(), "int tgm_rail_down(void)"));
	expectBefore(down, "sleep_gate();", "tgm_deadline_suspend();");
	expectBefore(down, "tgm_deadline_suspend();", "sleep_fences();");
	expectBefore(flat(definition(gpu(), "void tgm_rail_up_locked(const void *site)")),
		     "__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);",
		     "tgm_deadline_resume();");
	const std::string power = flat(definition(gpu(), "static void sleep_power_down(void)"));
	expectBefore(power, "nvxnu_irq_suspend();", "tgm_recover_suspend();");
	expectBefore(power, "tgm_recover_suspend();", "regulator_disable(");
}

// IOGPUDeviceSetDisplayParams is scalar method 0x21 in iOS 17's IOGPU.framework and IOConnectTrap2 index 8 in iOS 27's;
// XNU answers a trap index with no trap behind it kIOReturnSuccess, so the device connection serves index 8 and refuses
// every other by name instead of letting it pass unseen.
TEST(GM20BDeviceSource, DisplayParamsArriveByMethodAndByTrap)
{
	const std::string client = SourceText("../TegraGM20BDeviceClient.cpp");
	EXPECT_THAT(
		flat(definition(client, "IOExternalTrap "
					"*TegraGM20BDeviceUserClient::getTargetAndTrapForIndex(")),
		HasSubstr("*targetP = this; if (index == kSetDisplayParamsTrap) return "
			  "&displayParams; switch_refusal_k("));
	EXPECT_THAT(flat(client), HasSubstr("constexpr UInt32 kSetDisplayParamsTrap = 8;"));
	EXPECT_THAT(
		flat(definition(client, "IOReturn TegraGM20BDeviceUserClient::setDisplayParams(")),
		HasSubstr("storeDeadline(reinterpret_cast<uint64_t>(nextVblank), "
			  "reinterpret_cast<uint64_t>(interval));"));
	EXPECT_THAT(flat(client),
		    HasSubstr("case kSetDisplayParams: if (arguments->scalarInputCount != "
			      "2) return kIOReturnBadArgument; storeDeadline("));
}

// A hang's line reads the hung submission's pushes through the user client that claimed the channel's space, and only
// that: the kernel's copiers (the tiling move, the copy engine's plane copies) claim theirs with their own state, which
// is no AGXDeviceUserClient. A live channel's client is not freed under it: hang recovery describes channels on the
// live list under tgm_live_lock, and the client's free() takes every channel off it before it frees its lock.
TEST(GM20BHangSource, OnlyAConnectionsPushesAreRead)
{
	EXPECT_THAT(flat(definition(gpu(), "void tgm_chan_describe(")),
		    HasSubstr("CHAN_PUSH_BYTES, chan->vm->connection, note, sizeof(note));"));
	EXPECT_THAT(flat(definition(client(), "bool AGXDeviceUserClient::setUp(")),
		    HasSubstr("tgm_vm_claim_connection(vm_, this);"));
	EXPECT_THAT(client(), Not(HasSubstr("tgm_vm_claim(")));
	// The kernel copy queues' spaces are claimed by their producers' state, never as a connection's.
	EXPECT_THAT(flat(definition(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")),
		    HasSubstr("tgm_vm_claim(vm, owner);"));
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	EXPECT_THAT(flat(relayout), HasSubstr("r.space.openSpace(kShape, &gRelayout)"));
	EXPECT_THAT(relayout, Not(HasSubstr("tgm_vm_claim_connection(")));
	const std::string copy = SourceText("../TegraGM20BCopy.cpp");
	EXPECT_THAT(flat(definition(copy, "bool openSpace()")), HasSubstr("queue.space.openSpace(kShape, &queue)"));
	EXPECT_THAT(copy, Not(HasSubstr("tgm_vm_claim_connection(")));
	expectBefore(flat(definition(client(), "void AGXDeviceUserClient::free()")),
		     "drainResources();", "IOLockFree(lock_);");
	expectBefore(flat(definition(client(), "void AGXDeviceUserClient::drainResources()")),
		     "for (auto &chan : chans_) tgm_chan_del(&chan);", "thread_call_cancel_wait(retireCall_);");
	expectBefore(flat(definition(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)")),
		     "mutex_lock(&tgm_live_lock); list_del(&chan->live);", "chan_free(chan);");
}

// The tiling move and the copy engine's plane copies follow the surface's running writers through tgm_relayout_waits:
// acquires in their own push (TegraGM20BRelayout.h tgm_relayout_follow), a refusal for one they cannot acquire on, never a
// CPU wait. The kernel's own client channels' fences sit in the fence view's last slot, which every space maps and the
// connections' allocator never hands out, so a copy can acquire on the copy engine's writes.
TEST(GM20BRelayoutSource, KernelCopiesOrderOnTheGpu)
{
	const std::string relayout = SourceText("../TegraGM20BRelayout.cpp");
	const std::string waits =
		flat(definition(relayout, "extern \"C\" uint32_t tgm_relayout_waits("));
	EXPECT_THAT(waits,
		    HasSubstr("nwaits = tgm_relayout_follow(self, running, fenceVa, n, waits, "
			      "nwaits, &refused);"));
	EXPECT_THAT(
		waits,
		HasSubstr("\"gpu-order\", \"writer-unordered\", \"a kernel copy cannot acquire"));
    EXPECT_THAT(waits, HasSubstr("return TGM_RELAYOUT_WAIT_FAILED;"));
    EXPECT_THAT(waits, Not(HasSubstr("tgm_writer_wait(")));
	const std::string move = flat(definition(relayout, "int move("));
	EXPECT_THAT(move,
		    HasSubstr("tgm_relayout_waits(r.space.vm, tgm_chan_uid(chan), writers, nwriters, "
			      "waits, 0)"));
	// its wait is late on the writer an acquire sits on, so recovery finds that writer, not the copy behind it
	EXPECT_THAT(move, HasSubstr("acquired[i] = {waits[i].uid, waits[i].seqno};"));
	EXPECT_THAT(move, HasSubstr("tgm_copy_wait(tgm_chan_uid(chan), seqno, acquired, nwaits);"));
	EXPECT_THAT(move, Not(HasSubstr("tgm_writer_wait(")));
	const std::string late = flat(definition(gpu(), "static struct tgm_chan *late_writer("));
	EXPECT_THAT(late, HasSubstr("(s32)(tgm_chan_completed(chan) - acquired[i].seqno) < 0"));
	EXPECT_THAT(late, HasSubstr("*late_seqno = seqno; return live_by_uid(uid);"));
	EXPECT_THAT(
		flat(definition(gpu(), "static void writer_wait(")),
		HasSubstr("chan = late_writer(uid, seqno, acquired, nacquired, &late); if "
			  "(chan) verdict = tgm_hang_recover(chan, \"IOSurface lock\", late, "));
	EXPECT_THAT(flat(relayout), HasSubstr("{\"relayout\", TGM_FENCE_KERNEL_RELAYOUT, 1, kPushVa, 0, 1}"));
	EXPECT_THAT(flat(definition(SourceText("../SwitchGpuKqueue.h"), "bool openSpace(")),
		    HasSubstr("tgm_fence_kernel(shape.fenceSlot, &fenceCpu, &fencePage, &fenceOffset)"));
	const std::string copy = flat(SourceText("../TegraGM20BCopy.cpp"));
	EXPECT_THAT(copy, HasSubstr("switch_gpu_backing_dependencies(&transaction, queue.dependencies,"));
	EXPECT_THAT(copy, HasSubstr("queue.events.captureDependencies(queue.dependencies, count)"));
	EXPECT_THAT(copy, HasSubstr("switch_gpu_chan_exec_visibility("));
	EXPECT_THAT(copy, Not(HasSubstr("tgm_writer_wait(")));
	EXPECT_THAT(copy, HasSubstr("{\"ce-copy\", TGM_FENCE_KERNEL_COPY, Jobs, PushVa, 1, 0}"));
	const std::string fences = flat(gpu());
	EXPECT_THAT(fences, HasSubstr("#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)"));
	EXPECT_THAT(flat(definition(
			    gpu(), "int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)")),
		    HasSubstr("i = (tgm_fence_pages.next + n) % TGM_FENCE_USER_PAGES;"));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_fence_kernel(")),
		    HasSubstr("TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct "
			      "TegraGM20BAbiFence);"));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_writer_fence_va(")),
		    HasSubstr("off < TGM_FENCE_BYTES"));
}

TEST(GM20BSurfaceIdentitySource, ImportedSurfaceIdentityOutlivesLegacyCoherency)
{
	const auto imported = flat(definition(client(), "IOReturn AGXDeviceUserClient::boImport("));
	expectBefore(imported, "bo->surface = switch_gpu_surface_hold(", "tgm_coherency_add(");
	const auto released = flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)"));
	expectBefore(released, "tgm_coherency_put(bo->coh);", "OSSafeReleaseNULL(bo->surface);");
}

// A held command buffer's writer promise (TegraGM20BMethodWriterPromise) is the connection's: method 34 reaches it under
// the connection lock with the submit gate open, it names writes of the connection's IOSurface imports only, and each
// release from the CPU reaches a parked GPU reader through the write-combined fence page.
TEST(GM20BJobSource, AWriterPromiseIsTheConnectionsAndEndsWithIt)
{
	const std::string run = flat(definition(client(), "IOReturn AGXDeviceUserClient::run(uint32_t method,"));
	EXPECT_THAT(run, HasSubstr("case TegraGM20BMethodWriterPromise: wantIn = sizeof(TegraGM20BAbiWriterPromise); "
				   "wantOut = sizeof(TegraGM20BAbiWriterPromiseReply);"));
	EXPECT_THAT(run, HasSubstr("submitGate_.closed() ? kIOReturnNotOpen : writerPromise("));
	const std::string promises = SourceText("../TegraGM20BUserClientPromise.cpp");
	const std::string promise = flat(definition(
		promises, "IOReturn AGXDeviceUserClient::writerPromise(const TegraGM20BAbiWriterPromise *in,"));
	EXPECT_THAT(promise, HasSubstr("if (!bo || !bo->backing || !bo->surface || uses[i].access != TegraGM20BSurfaceWrite"));
	EXPECT_THAT(promise, Not(HasSubstr("switch_gpu_promise_release(")));
	const std::string release =
		flat(definition(promises, "void AGXDeviceUserClient::releasePromise(uint32_t promise, bool abandoned)"));
	expectBefore(release, "tgm_promise_abandon(offset, value);",
		     "switch_gpu_promise_release(&promises_[promise], promiseWord(promise))");
	expectBefore(release, "switch_gpu_promise_release(&promises_[promise], promiseWord(promise))",
		     "__asm__ volatile(\"dsb sy\" ::: \"memory\");");
	expectBefore(flat(definition(client(), "void AGXDeviceUserClient::drainResources()")), "releasePromises(true);",
		     "tgm_chan_del(&chan);");
}

// A direct present whose producer still runs at the offer has no job (SwitchGpuPresentWatch.h): the producer's own
// interrupt pass makes it due with plain loads, the pass's end delivers it with interrupts on, and a kill, a hidden
// producer and the deadline go to the work item. The arm comes before the offer's own look.
TEST(GM20BPresentSource, AWatchedPresentIsCompletedByItsProducersInterruptPass)
{
	const auto fenced = flat(definition(gpu(), "static inline void present_watch_fenced("));
	EXPECT_THAT(fenced, HasSubstr("switch_gpu_present_watch_fenced(&tgm_present.watch, chan->id, "
				      "tgm_chan_completed(chan))"));
	EXPECT_THAT(fenced, Not(HasSubstr("mutex_lock")));
	EXPECT_THAT(fenced, Not(HasSubstr("finish_visible")));
	const std::string killed = flat(definition(
		gpu(), "int tgm_chan_killed(struct nvif_event *event, void *repv, u32 repc)"));
	expectBefore(killed, "__atomic_store_n(chan->fence_cpu, chan->submitted, __ATOMIC_RELEASE);",
		     "present_watch_fenced(chan, 1);");
	const std::string pass = flat(definition(
		SourceText("../shim/xnu_glue.cpp"),
		"void gpuInterrupt(OSObject *, IOInterruptEventSource *source, int)"));
	expectBefore(pass, "nvxnu_poll_irqs();", "tgm_present_pass();");
	expectBefore(pass, "tgm_present_pass();", "nvxnu_wake();");
	const std::string deliver = flat(definition(gpu(), "void tgm_present_pass(void)"));
	EXPECT_THAT(deliver, HasSubstr("switch_gpu_present_watch_take_visible("));
	// the pass's own thread: nothing may wait for a GPU interrupt
	EXPECT_THAT(deliver, HasSubstr("switch_gpu_present_tokens_finish_ready("));
	EXPECT_THAT(deliver, Not(HasSubstr("finish_visible")));
	EXPECT_THAT(deliver, Not(HasSubstr("_wait(")));
	EXPECT_THAT(flat(Between(SourceText("../SwitchGpuPresentTokens.h"),
				 "switch_gpu_present_tokens_finish_ready(", "\n}\n")),
		    Not(HasSubstr("token_wait")));
	EXPECT_THAT(deliver, HasSubstr("nvxnu_work_schedule(tgm_present.watch_work);"));
	EXPECT_THAT(deliver, Not(HasSubstr("mutex_lock")));
	const std::string arm =
		flat(definition(gpu(), "int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,"));
	expectBefore(arm, "switch_gpu_present_watch_arm(", "mutex_unlock(&tgm_present.lock);");
	expectBefore(arm, "gate_leave();", "present_physical(&tgm_present.watch.token)");
	// every producer is asked, of the channel its token names, between the arm and the last look: the job that keeps a
	// writer promise in the promise's place once it is submitted
	const char *const ask = "tgm_chan_need_tokens(kept.engine ? &kept : dependencies, kept.engine ? 1 : dependency_count);";
	expectBefore(arm, "switch_gpu_present_watch_arm(", ask);
	expectBefore(arm, "gate_leave();", ask);
	expectBefore(arm, "tgm_present_promise_watch(&kept);", "switch_gpu_present_watch_promise(");
	// the job that keeps a watched promise takes its place, retained before the watch is handed over
	const std::string kept = flat(definition(
		SourceText("../gpu_promise.c"),
		"void tgm_present_promise_kept(uint32_t offset, uint32_t value, const struct switch_gpu_backing_token *producer)"));
	expectBefore(kept, "!switch_gpu_backing_token_retain(producer)", "switch_gpu_present_watch_kept(");
	expectBefore(kept, "switch_gpu_present_watch_rearm(&tgm_present.watch);", "tgm_chan_need_tokens(producer, 1);");
	expectBefore(kept, "tgm_chan_need_tokens(producer, 1);", "switch_gpu_backing_token_physical_status(producer, 0)");
	expectBefore(arm, "tgm_chan_need_tokens(", "present_physical(&tgm_present.watch.token)");
	EXPECT_THAT(arm, Not(HasSubstr("tgm_chan_need_uid(")));
	EXPECT_THAT(arm, HasSubstr("nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);"));
	const std::string outstanding = flat(definition(gpu(), "int tgm_present_outstanding(const void *arg)"));
	EXPECT_THAT(outstanding, HasSubstr("switch_gpu_present_watch_armed(&tgm_present.watch)"));
	// the deadline never delivers a watch: it follows the work an asked interrupt is behind, or panics for a lost one
	const std::string deadline = flat(definition(gpu(), "void tgm_present_deadline(void *arg)"));
	EXPECT_THAT(deadline, Not(HasSubstr("present_watch_work(")));
	expectBefore(deadline, "watched = switch_gpu_backing_token_physical_status(&tgm_present.watch.token, 0);",
		     "mutex_lock(&tgm_live_lock);");
	expectBefore(deadline, "if (watch_late(watched, age, &waited, &seqno)) {", "tgm_hang_recover(");
	const std::string late = flat(definition(gpu(), "static int watch_late("));
	expectBefore(late, "switch_gpu_quiet_interrupt(", "nvxnu_panic(");
	EXPECT_THAT(late, HasSubstr("if (now - tgm_present.late < TGM_PRESENT_WAKE_GRACE_NS) {"));
	EXPECT_THAT(late, Not(HasSubstr("watch_claim")));
	EXPECT_THAT(late, Not(HasSubstr("watch_work")));
	EXPECT_THAT(flat(definition(gpu(), "int tgm_present_watch_dependencies(")),
		    HasSubstr("tgm_present.late = 0;"));
	const std::string del = flat(definition(gpu(), "void tgm_chan_del(struct tgm_chan **pchan)"));
	expectBefore(del, "chan_abandon(chan);", "present_watch_fenced(chan, 1);");
	expectBefore(del, "present_watch_fenced(chan, 1);", "list_del(&chan->live);");
	EXPECT_THAT(deadline, HasSubstr("if (tgm_present.pending && (s32)(tgm_chan_completed(tgm_present.chan) - "
					"tgm_present.pending) >= 0)"));
	const std::string offer = flat(definition(client(), "extern \"C\" int tegra_gm20b_scanout_copy("));
	EXPECT_THAT(offer, HasSubstr("ret = watch && !nreads ? presentWatched() : 1;"));
	EXPECT_THAT(offer, HasSubstr("ret = watch ? presentWatched() : 1;"));
}

// A context-switch timeout on the present's channel resets the copy engine and copies the frame again on the GPU: the cut
// present's completion waits for the redo, so the display never gets a partial-frame status and never copies on the CPU.
// Scrub jobs a reset cut go back dirty.
TEST(GM20BPresentSource, ACopyEngineResetRedoesThePresentOnTheGpu)
{
	const auto fenced = flat(definition(gpu(), "static inline void present_fenced("));
	expectBefore(fenced,
		     "redo = pending; if (__atomic_compare_exchange_n(&tgm_present.redo, &redo, 0,",
		     "nvxnu_work_schedule(tgm_present.completion_work);");
	expectBefore(fenced, "__atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE)) return;",
		     "redo = pending; if (");
	expectBefore(fenced, "nvxnu_work_schedule(tgm_present.redo_work); return; }",
		     "nvxnu_work_schedule(tgm_present.completion_work);");
	const auto redo = flat(definition(gpu(), "void tgm_present_redo_work("));
	expectBefore(redo, "|| !gate_try()) return;", "tgm_present_push_copy(push);");
	expectBefore(redo, "tgm_present_push_copy(push);", "chan_fence_volatile(chan);");
	expectBefore(redo, "__atomic_store_n(&tgm_present.redoing, 0, __ATOMIC_RELEASE);",
		     "#endif mutex_unlock(&tgm_present.lock); gate_leave();");
	EXPECT_THAT(flat(definition(gpu(), "static void gate_open(")),
		    HasSubstr("tgm_present_redo_kick();"));
	EXPECT_THAT(redo, Not(HasSubstr("done(arg")));
	EXPECT_THAT(redo, Not(HasSubstr("tgm_present.done")));
	EXPECT_THAT(redo, Not(HasSubstr("SEMAPHORED")));
	// a redo with no room in the ring goes to the hang rule and is tried again, never a panic
	expectBefore(redo, "tgm_chan_stalled(chan, \"present redo: push buffer still full after 1 s\"",
		     "nvxnu_work_schedule(tgm_present.redo_work);");
	EXPECT_THAT(redo, Not(HasSubstr("nvxnu_panic(")));
	const auto mark = flat(definition(gpu(), "int tgm_present_redo("));
	expectBefore(mark, "__atomic_store_n(&tgm_present.redo, pending, __ATOMIC_SEQ_CST);",
		     "if (__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) == pending) "
		     "return 1;");
	EXPECT_THAT(flat(definition(gpu(), "static inline void scrub_fenced(")),
		    HasSubstr("nvxnu_pool_free_dirty(tgm_scrub.job[slot].pa, "
			      "tgm_scrub.job[slot].bytes);"));
	const auto decide = flat(definition(gpu(), "enum tgm_ctxsw tgm_ctxsw_decide("));
	// a stuck kernel channel never panics here: tgm_kernel_stall_locked decides, and marks the redo before the reset
	EXPECT_THAT(decide, HasSubstr("if (chan->kernel && (verdict == TGM_CTXSW_REDO || verdict == TGM_CTXSW_FATAL)) "
				      "verdict = tgm_kernel_stall_locked(chan, engine_name, verdict == TGM_CTXSW_FATAL, 0, "
				      "1, &killed);"));
	EXPECT_THAT(decide, Not(HasSubstr("nvxnu_panic(")));
	const auto stall = flat(definition(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked("));
	expectBefore(stall, "if (verdict == TGM_CTXSW_FATAL) {", "*killed = kill_clients(");
	expectBefore(stall, "*killed = kill_clients(", "tgm_ctxsw_redo_mark(chan);");
	EXPECT_THAT(stall, HasSubstr("if (!fatal_ok) return verdict;"));
	// an idle channel the engine cannot switch off has no drain to wait for: a kernel one is reset, a client's killed,
	// at the first tick
	EXPECT_THAT(decide, HasSubstr("chan_stuck_for(chan, now, tgm_stuck_window(gr)) || chan_unowned(chan)"));
	// progress after the reset: the redone work landing, or with nothing to redo any job completing
	EXPECT_THAT(flat(definition(gpu(), "void tgm_ctxsw_redo_mark(")),
		    HasSubstr("chan->ctxsw_reset_after = marked ? chan->submitted : "
			      "tgm_chan_completed(chan);"));
}

// A context load an engine is stuck in again after a reset under it: a client's is that client's alone and kills it, a
// kernel channel's goes to the kernel stall rule, whose only panic is the engine that did not come back
TEST(GM20BRecoverSource, ASecondStuckLoadKillsTheClientOrGoesToTheKernelStallRule)
{
	const auto load = flat(definition(gpu(), "static enum tgm_ctxsw load_verdict("));
	expectBefore(load,
		     "if (chan->kernel) return tgm_kernel_stall_locked(chan, engine, verdict == TGM_CTXSW_FATAL, 1, "
		     "1, &killed);",
		     "tgm_chan_isolate(chan, report, nvxnu_nanotime());");
	expectBefore(load, "tgm_chan_isolate(chan, report, nvxnu_nanotime());", "return TGM_CTXSW_KILL;");
	EXPECT_THAT(load, Not(HasSubstr("nvxnu_panic(")));
	const auto stall = flat(definition(gpu(), "enum tgm_ctxsw tgm_kernel_stall_locked("));
	EXPECT_EQ(Count(stall, "nvxnu_panic("), 1u);
	EXPECT_THAT(stall, HasSubstr("hardware failure: %s did not come back after a reset with no client "));
	EXPECT_THAT(stall, HasSubstr("chan->ctxsw_clean_ns = n ? 0 : now;"));
	EXPECT_THAT(flat(definition(gpu(), "static u32 kill_clients(")),
		    HasSubstr("tgm_chan_isolate(clients[i], report, now); "
			      "tgm_recover_fault(tgm_gpu.device, (int)clients[i]->chid);"));
}

TEST(GM20BPresentSource, TypedProducerValidationAndReleaseNeverRunUnderTheIrqEventLock)
{
	const auto irq = flat(definition(gpu(), "static inline void present_fenced("));
	EXPECT_THAT(irq, HasSubstr("nvxnu_work_schedule(tgm_present.completion_work)"));
	EXPECT_THAT(irq, Not(HasSubstr("switch_gpu_present_tokens_finish")));
	const auto complete = flat(definition(gpu(), "static void present_typed_complete("));
	expectBefore(complete, "mutex_unlock(&tgm_present.lock);",
		     "switch_gpu_present_tokens_finish_visible(");
	expectBefore(complete, "switch_gpu_present_tokens_finish_visible(",
		     "&tgm_present.typed_pending, 0");
	const auto copy = flat(definition(gpu(), "static int present_copy("));
	expectBefore(
		copy,
		"tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count, 1)",
		"if (dst_va) tgm_present_push_copy(push);");
	expectBefore(copy, "&tgm_present.typed_pending, 1", "chan_fence(chan);");
	EXPECT_THAT(copy, HasSubstr("dependency_count * 5"));
	EXPECT_THAT(copy, HasSubstr("push->cur = push_start"));
}

TEST(GM20BPresentSource, ExactFrameSkipsLiveChannelEnumerationEvenWithoutGpuTokens)
{
    const auto copy = flat(definition(gpu(), "static int present_copy("));
    EXPECT_THAT(copy, Not(HasSubstr("exact")));
    EXPECT_THAT(copy, Not(HasSubstr("list_for_each_entry")));
    const auto exact = flat(definition(gpu(), "int tgm_present_copy_dependencies("));
    EXPECT_THAT(exact, HasSubstr("dependencies, dependency_count);"));
    const auto scanout = flat(definition(client(), "extern \"C\" int tegra_gm20b_scanout_copy("));
    EXPECT_THAT(scanout, HasSubstr("gPresentSnapshot.add(sourceBacking)"));
    EXPECT_THAT(scanout, HasSubstr("gPresentSnapshot.tokens, gPresentSnapshot.count"));
}

TEST(GM20BVmSource, MappingSerializesPerAddressSpaceWithSuspendAdmission)
{
	for (const char *signature :
	     {"int tgm_vm_map_raw(", "int tgm_vm_unmap_pages(", "int switch_gpu_vm_map_volatile(",
	      "void switch_gpu_vm_unmap(", "int tgm_vm_kmap_raw("}) {
		const auto body = flat(definition(gpu(), signature));
		expectBefore(body, "gate_enter();", "mutex_lock(&vm->map_lock);");
		expectBefore(body, "mutex_unlock(&vm->map_lock);", "gate_leave();");
		EXPECT_THAT(body, Not(HasSubstr("gpu_lock();")));
		EXPECT_THAT(body, Not(HasSubstr("mem_prepare_big_views(")));
	}
	const auto memory = flat(definition(gpu(), "static int mem_new("));
	expectBefore(memory, "gpu_lock();", "mem_prepare_big_views(mem);");
	expectBefore(memory, "mem_prepare_big_views(mem);", "mutex_unlock(&tgm_gpu.lock);");
	EXPECT_THAT(flat(definition(gpu(), "void tgm_mem_del(")), HasSubstr("gpu_lock();"));
	EXPECT_THAT(flat(definition(gpu(), "void tgm_vm_del(")), HasSubstr("gpu_lock();"));
}

// A command buffer's batches are one SubmitAddress (TegraGM20BPushFence): its uses are owned against the seqno of the
// job's own fence, after every internal one (switch_gpu_job_fences_test runs the emission order).
TEST(GM20BJobFencesSource, OneJobOwnsItsUsesUntilItsLastFence)
{
	const auto exec = flat(definition(client(), "IOReturn AGXDeviceUserClient::exec("));
	EXPECT_THAT(exec, HasSubstr("const uint32_t fenceFlag = native && scratch ? uint32_t(TegraGM20BPushFence) : 0u;"));
	EXPECT_THAT(exec, HasSubstr("if (switch_gpu_job_fences(pushes, count, &fences)) return kIOReturnBadArgument;"));
	EXPECT_THAT(exec, HasSubstr("switch_gpu_chan_job_token(chan, fences, &completion);"));
	EXPECT_THAT(exec, Not(HasSubstr("switch_gpu_chan_next_token(")));
	// resolved once: a written import is marked from what the uses resolved, not looked up again
	EXPECT_THAT(exec, HasSubstr("job.written[writtenCount++] = bo->coh;"));
	EXPECT_THAT(exec, HasSubstr("tgm_coherency_wrote_listed(job.written[i], tgm_chan_uid(chan), seqno);"));
	EXPECT_THAT(exec, Not(HasSubstr("findAddress(")));
	const auto emit = flat(definition(gpu(), "int chan_exec_body("));
	const auto room = flat(definition(gpu(), "static void chan_submission_room("));
	EXPECT_THAT(
		room,
		HasSubstr("*entries = count + 6 + stamps + job_fences + CHAN_WAKE_ENTRIES + 1;"));
	EXPECT_THAT(
		room,
		HasSubstr("SWITCH_GPU_FENCE_POINT_DWORDS * job_fences + CHAN_WAKE_DWORDS + 16 +"));
	EXPECT_THAT(emit, HasSubstr("last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);"));
	EXPECT_THAT(flat(definition(client(), "uint32_t switch_gpu_info_flags(void)")),
		    HasSubstr("TegraGM20BInfoPushFences;"));
}

// A use's handle is found by the index (switch_gpu_bo_index_test), never by walking the connection's BOs; every BO
// that joins bos_ joins the index, and every one that leaves it leaves the index.
TEST(GM20BBoIndexSource, EveryBoListChangeKeepsTheIndex)
{
	const auto find = flat(definition(client(), "AGXDeviceUserClient::Bo *AGXDeviceUserClient::findBo("));
	EXPECT_THAT(find, HasSubstr("return boIndex_.find(handle);"));
	const std::string sources = client() + SourceText("../SwitchGpuNativeImport.h") +
				    SourceText("../SwitchGpuClientRetirement.h");
	EXPECT_EQ(Count(sources, "bos_ = bo;"), Count(sources, "boIndex_.insert(bo->handle, bo);"));
	EXPECT_EQ(Count(sources, "boIndex_.reserve()"), 3u);
	expectBefore(flat(definition(SourceText("../SwitchGpuClientRetirement.h"), "IOReturn AGXDeviceUserClient::boFree(")),
		     "*p = bo->next;", "boIndex_.erase(bo->handle);");
}

TEST(GM20BVisibilitySource, InternalFenceKeepsExecutionOrderAndExternalFencePublishesVisibleFirst)
{
	const auto fence = flat(definition(gpu(), "static u32 chan_fence_job("));
	EXPECT_THAT(fence, HasSubstr("if (external && (l2_on() || chan->kernel))"));
	expectBefore(fence, "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "if (external && chan->visible_cpu)");
	expectBefore(fence, "lower_32_bits(chan->visible_gpu)", "lower_32_bits(chan->fence_gpu)");
	EXPECT_THAT(fence, HasSubstr("RELEASE_WFI, EN"));
	const auto exec = flat(definition(gpu(), "int chan_exec_body("));
	EXPECT_THAT(exec, HasSubstr("if ((l2_on() && host_written) || tlb)"));
	expectBefore(exec, "NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY", "NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE");
	EXPECT_THAT(
		exec,
		HasSubstr(
			"*seqno = chan_fence_job(chan, external, chan->submitted + 1, last, quiet, "
			"releases, release_count);"));
	// A batch's internal fence orders execution only: no L2 flush, no visible release, no interrupt. Its kick is the
	// GPFIFO entry that keeps it between the batches; the job's own kick writes GP_PUT.
	const auto internal = flat(definition(gpu(), "static void chan_job_fence("));
	EXPECT_THAT(internal, HasSubstr("RELEASE_WFI, EN"));
	EXPECT_THAT(internal, Not(HasSubstr("L2_FLUSH_DIRTY")));
	EXPECT_THAT(internal, Not(HasSubstr("visible_gpu")));
	EXPECT_THAT(internal, Not(HasSubstr("NON_STALL_INTERRUPT")));
	EXPECT_THAT(internal, Not(HasSubstr("defer_kick")));
	expectBefore(internal, "RELEASE_SIZE, 4BYTE));", "PUSH_KICK(push);");
	expectBefore(exec, "chan->chan.defer_kick = true;", "last = switch_gpu_job_emit(");
	expectBefore(fence, "chan->chan.defer_kick = false;", "PUSH_KICK(push);");
	expectBefore(
		fence,
		"job = switch_gpu_jobs_kick(&chan->jobs, first, next, chan->submit_ns, !announce);",
		"PUSH_KICK(push);");
	for (const char *signature : {"int tgm_vm_unmap_pages(", "void switch_gpu_vm_unmap("}) {
		const auto unmap = flat(definition(gpu(), signature));
		expectBefore(unmap, "nvkm_ltc_flush(tgm_gpu.device->ltc)", "nvif_vmm_raw_unmap(");
	}
}

TEST(GM20BVisibilitySource, ColdGlobalInvalidationPreservesInternalDirtyData)
{
    const auto body = flat(definition(SourceText("../nouveau/nvkm/subdev/ltc/base.c"),
        "nvkm_ltc_invalidate(struct nvkm_ltc *ltc)"));
    expectBefore(body, "mutex_lock(&ltc->mutex)", "ltc->func->flush(ltc)");
    expectBefore(body, "ltc->func->flush(ltc)", "ltc->func->invalidate(ltc)");
    expectBefore(body, "ltc->func->invalidate(ltc)", "mutex_unlock(&ltc->mutex)");
}

// nvkm_runl_chan_get_chid returns with spinlocks held and interrupts off; nvkm_kmap takes ltc->mutex.
TEST(GM20BChannelSource, RamfcIsMappedOnlyAfterTheChannelLookupUnlocks)
{
    const auto lookup = flat(definition(gpu(), "static struct nvkm_gpuobj *chan_inst("));
    expectBefore(lookup, "nvkm_runl_chan_get_chid(", "nvkm_chan_put(&nchan, flags)");
    for (const char *signature : {"void tgm_ramfc_write(", "static int ramfc_read_waits("}) {
        const auto body = flat(definition(gpu(), signature));
        expectBefore(body, "chan_inst(chid)", "nvkm_kmap(");
        expectBefore(body, "nvkm_kmap(", "nvkm_done(");
    }
    EXPECT_THAT(flat(definition(gpu(), "static void chan_timeslice(")), HasSubstr("tgm_ramfc_write("));
}

TEST(GM20BChannelSource, CopyChannelCreatesOnlyRequestedObjectsAndKeepsVerifiedRunlist)
{
    const auto create = flat(definition(gpu(), "static int chan_new("));
    EXPECT_THAT(create, HasSubstr("NV_DEVICE_HOST_RUNLIST_ENGINES_GR"));
    EXPECT_THAT(create, HasSubstr("args.chan.runq = 0"));
    EXPECT_THAT(create, HasSubstr("i = switch_gpu_channel_first_object(copy_only); i < 5"));
    EXPECT_THAT(create, HasSubstr("switch_gpu_channel_bind_words(bind_words, classes, copy_only)"));
    EXPECT_THAT(create, HasSubstr("chan_push_wait(chan, bind_count)"));
    EXPECT_THAT(create, HasSubstr("PUSH_DATA(push, bind_words[i])"));
    const auto objects = Between(create, "i = switch_gpu_channel_first_object(copy_only)", "host->version = 0");
    expectBefore(objects, "if (ret)", "goto fail");
    EXPECT_THAT(gpu(), HasSubstr("tgm_chan_new_copy(tgm_present.vm"));
    EXPECT_THAT(gpu(), HasSubstr("tgm_chan_new_copy(tgm_scrub.vm"));
    EXPECT_THAT(SourceText("../SwitchGpuKqueue.h"), HasSubstr("tgm_chan_new_copy(vm, owner"));
    for (const char *producer : {"../TegraGM20BCopy.cpp", "../TegraGM20BRelayout.cpp", "../SwitchGpuCachedZero.cpp",
				 "../SwitchGpuBridge.cpp"}) {
        EXPECT_THAT(SourceText(producer), HasSubstr("space.openChannel(kShape"));
        EXPECT_THAT(SourceText(producer), Not(HasSubstr("tgm_chan_new_copy(")));
    }
}

// Every channel's acquire on another channel's fence reads the address space's view of the fence pool: a space without
// it would refuse every cross-channel dependency for its life, so it is not made.
TEST(GM20BVmSource, AnAddressSpaceWithoutItsFenceViewIsNotMade)
{
	const std::string make = flat(definition(gpu(), "int tgm_vm_new(struct tgm_vm **pvm)"));
	expectBefore(make, "ret = tgm_fences_map(vm);", "tgm_vm_del(&vm);");
	EXPECT_THAT(make, Not(HasSubstr("vm->fences_va = 0;")));
}

// A client unmaps a pool BO after its BoFree: the pages wait for that unmap, which releases them, and are not a leak.
TEST(GM20BPoolSource, FreedPoolPagesGoWithTheirLastMapping)
{
	EXPECT_THAT(flat(definition(client(), "void AGXDeviceUserClient::releaseBo(Bo *bo)")),
		    Not(HasSubstr("still mapped by its")));
	for (const char *signature : {"IOReturn AGXDeviceUserClient::unmapToken(",
				      "void AGXDeviceUserClient::releaseMappings()"})
		EXPECT_THAT(flat(definition(client(), signature)),
			    HasSubstr("scheduleOrphanRelease();"));
}

// Pool allocations take zeroed pages only (nvxnu_pool_alloc_ready, chunks): the refusal rules count those.
TEST(GM20BPoolSource, PoolRefusalsCountOnlyZeroedFreePages)
{
	EXPECT_THAT(flat(definition(gpu(), "uint64_t tgm_pool_avail(void)")),
		    HasSubstr("nvxnu_pool_ready_pages()"));
}

// A layout decision may move the surface on the copy engine and wait for it: the connection's lock and mutation gate
// are not held meanwhile, and what it uses is held for it.
TEST(GM20BLayoutSource, LayoutDecisionHoldsNeitherTheConnectionLockNorItsGate)
{
	const std::string info =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo("));
	expectBefore(info, "tgm_coh_hold_asker(coh);", "IOLockUnlock(lock_);");
	expectBefore(info, "IOLockUnlock(lock_);", "tgm_coherency_layout(coh,");
	expectBefore(info, "tgm_coh_put_asker(coh);", "IOLockLock(lock_);");
	const std::string run = flat(definition(client(), "IOReturn AGXDeviceUserClient::run("));
	EXPECT_THAT(run, HasSubstr("method != TegraGM20BMethodSurfaceLayout &&"));
	EXPECT_THAT(run, HasSubstr("method != TegraGM20BMethodSurfaceLayoutInfo;"));
}

// The asker's hold (gm20b_layout_ask_test) is the only reference the decision does not count as another user: it is
// taken once and put once, with no return between them.
TEST(GM20BLayoutSource, TheAskersHoldIsPutOnEveryPath)
{
	const std::string info =
		flat(definition(client(), "IOReturn AGXDeviceUserClient::surfaceLayoutInfo("));
	EXPECT_EQ(Count(info, "tgm_coh_hold_asker("), 1u);
	EXPECT_EQ(Count(info, "tgm_coh_put_asker("), 1u);
	EXPECT_THAT(info, Not(HasSubstr("tgm_coh_hold(")));
	EXPECT_THAT(Between(info, "tgm_coh_hold_asker(coh);", "tgm_coh_put_asker(coh);"),
		    Not(HasSubstr("return")));
}

// nvgpu's gr_gm20b_init_fs_state writes ZROP's and CROP's settings whole with the L2 slice count (0x12006c,
// pri_ringmaster_enum_ltc), not the FBP count gk104's helper reads.
TEST(GM20BRopSource, ZropAndCropCountTheL2SlicesAsNvgpuDoes)
{
	const std::string gr = SourceText("../nouveau/nvkm/engine/gr/gm20b.c");
	const std::string init =
		flat(definition(gr, "gm20b_gr_init_rop_active_ltcs(struct gf100_gr *gr)"));
	EXPECT_THAT(init, HasSubstr("nvkm_rd32(device, 0x12006c)"));
	EXPECT_THAT(init, HasSubstr("nvkm_wr32(device, 0x408850, ltc_count);"));
	EXPECT_THAT(init, HasSubstr("nvkm_wr32(device, 0x408958, ltc_count);"));
	EXPECT_THAT(flat(gr), HasSubstr(".init_rop_active_fbps = gm20b_gr_init_rop_active_ltcs,"));
}
