"""The raw path's direct-compiled records of direct/exec.metal's states and kernels, executed on the host.

directrun (direct/directrun.mm) makes each record as the runtime makes it at a state's creation: m2v's translation,
NVK's stage compile, raw_direct.cpp's record. Each program then runs here (stage.py over interp.py), bound as the raw
path binds it (direct_records.py) and fed the words a draw or dispatch writes from the record's own fields (control
word, amplification id word, pulled stride words, stage-in region word). Every expected value is the Metal source's
arithmetic on the CPU, on inputs a zero or a stub would not pass: patch factors culled, clamped and kept, tessellation
coordinates off the corners, amplification 1, a stage-in origin past 0, a stride past 4095 bytes, buffers across a
4 GiB boundary, counter spans across 2^32, a fragment quad's storage image texels in each layer of a layered pass
(quad.py), every layer's its own, tessellation factors up to 64, eight amplifications, 32 float4 varyings, and a kernel
of 48 KiB of threadgroup memory under its QMD's shared memory size.

usage: direct_exec_test.py <direct_runner dir> <shader tools dir>
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from tools.gpu.maxwell import direct_records as dr
from tools.gpu.maxwell.interp import Inputs, UnsupportedError
from tools.gpu.maxwell.quad import run_quad

RECORDS: dict = {}
ATTR_LAYER, ATTR_VIEWPORT, ATTR_POSITION, ATTR_GENERIC = 0x64, 0x68, 0x70, 0x80
ATTR_TESS_COORD, ATTR_INSTANCE_ID, ATTR_VERTEX_ID = 0x2F0, 0x2F8, 0x2FC
TESS_LEVELS = {'outer': (0x000, 0x004, 0x008), 'inner': 0x010}
SV_LANE_ID, SV_INVOCATION_ID, SV_INVOCATION_INFO = 0x00, 0x11, 0x1D
SV_COMBINED_TID, SV_TID_X, SV_CTAID_X = 0x20, 0x21, 0x25
# a tessellation stage's one invocation of a patch of one vertex: SV_INVOCATION_INFO's byte 0 the patch in the batch,
# byte 2 its vertices (NAK's per-vertex input index, nak_nir_lower_vtg_io.c)
CONTROL_SYSTEM = {SV_LANE_ID: 0, SV_INVOCATION_ID: 0, SV_INVOCATION_INFO: 1 << 16}


def load(runner: Path, tools: Path) -> dict:
    work = Path(tempfile.mkdtemp(dir=os.environ.get('TEST_TMPDIR')))
    for name in ('directrun', 'libmock_iokit.dylib', 'exec.metallib'):
        shutil.copyfile(runner / name, work / name)
    (work / 'directrun').chmod(0o755)
    shutil.copyfile(tools / 'libvulkan_nouveau.dylib', work / 'libvulkan_nouveau.dylib')
    (work / 'cache').mkdir()
    out = work / 'records.json'
    made = subprocess.run(
        [work / 'directrun', work / 'libvulkan_nouveau.dylib', work / 'cache', work / 'exec.metallib', out],
        capture_output=True,
        text=True,
        check=False,
    )
    if made.returncode:
        raise RuntimeError(f'directrun: {made.stdout[-2000:]}{made.stderr[-4000:]}')
    return json.loads(out.read_text())


def outputs(environment: dr.Stage, base: int, count: int, vertex: int = 0) -> list[float]:
    return [dr.value(environment.outputs[(vertex, base + 4 * k)]) for k in range(count)]


def attributes(words: dict[int, int]) -> object:
    def attribute(address: int, _vertex: int, _patch: bool) -> int:
        if address not in words:
            raise UnsupportedError(f'ALD {address:#x}, an attribute the test gave no word for')
        return words[address]

    return attribute


class Records(unittest.TestCase):
    def state(self, name: str) -> dict:
        self.assertIn(name, RECORDS['states'], RECORDS['refused'].get(name, 'not made'))
        return RECORDS['states'][name]

    def kernel(self, name: str) -> dict:
        self.assertIn(name, RECORDS['kernels'], RECORDS['refused'].get(name, 'not made'))
        return RECORDS['kernels'][name]


class Tessellation(Records):
    """A triangle-patch state, m2v's vertex, control and evaluation stages.

    The vertex stage per patch, the control stage's levels and patch outputs from the factor buffer, and the
    post-tessellation vertex function at tessellation coordinates over the patch's control points.
    """

    NAME, TOP = 'tessellation', 16.0
    POINTS = tuple((float(k), float(2 * k + 1), float(-k), 1.0) for k in range(12))
    # MTLTriangleTessellationFactorsHalf per patch: three edge factors and the inside one
    FACTORS = ((2.0, 3.0, 4.0, 5.0), (40.0, 0.5, 6.0, 0.25), (0.0, 3.0, 3.0, 3.0), (70.0, 2.0, 3.0, 65.0))
    COORDS = ((0.25, 0.5), (0.5, 0.125), (0.0, 0.75))

    @staticmethod
    def levels(factors: tuple, top: float = 16.0) -> tuple[list[float], float | None]:
        """Give Metal's levels of a patch's factors under the integer partition.

        A patch of an edge factor at or below 0 is culled (every outer level 0, its inner one unread); each factor is
        clamped to [1, maxTessellationFactor].
        """
        edges, inside = factors[:3], factors[3]
        if any(not e > 0 for e in edges):
            return [0.0, 0.0, 0.0], None
        return [max(1.0, min(top, e)) for e in edges], max(1.0, min(top, inside))

    def setUp(self) -> None:
        self.record = self.state(self.NAME)
        self.assertEqual(len(self.record['tess_programs']), 2)
        self.binding = dr.Binding()
        self.binding.bind((1, 0), dr.floats(*[c for p in self.POINTS for c in p]))
        half = b''.join(struct.pack('<4e', *f) for f in self.FACTORS)
        self.binding.bind(('tess', 0), half)
        self.binding.bind(('tess', 1), bytes(256))
        self.binding.bind(('tess', 2), bytes(256))
        push = dr.record_push(self.record)
        first = self.record['control'][0] - 1
        self.assertGreaterEqual(first, 0)
        for k, word in enumerate([0, 0, dr.f32(1.0), 0, 0, 0]):
            push[first + k] = word
        self.binding.tables(dr.root_table(push, self.record['set_bytes']), dr.render_set(self.record, self.binding))

    def run_patch(self, patch: int) -> tuple[dr.Stage, dr.Stage]:
        vertex_program, _fragment = self.record['programs']
        control_program, _evaluation = self.record['tess_programs']
        system = {ATTR_VERTEX_ID: patch, ATTR_INSTANCE_ID: 0}
        vertex = dr.execute(vertex_program, dr.stage(vertex_program, self.binding, attributes(system), {}))
        written = {address: word for (_, address), word in vertex.outputs.items()}
        control = dr.stage(control_program, self.binding, attributes(written), CONTROL_SYSTEM)
        return vertex, dr.execute(control_program, control)

    def test_the_control_stage_writes_each_patchs_levels_from_its_factors(self):
        for patch, factors in enumerate(self.FACTORS):
            _, control = self.run_patch(patch)
            outer, inner = self.levels(factors, self.TOP)
            got = [dr.value(control.patch_outputs[a]) for a in TESS_LEVELS['outer']]
            self.assertEqual(got, outer, f'patch {patch}')
            if inner is not None:
                self.assertEqual(dr.value(control.patch_outputs[TESS_LEVELS['inner']]), inner, f'patch {patch}')

    def test_the_evaluation_stage_interpolates_the_patchs_control_points(self):
        evaluation_program = self.record['tess_programs'][1]
        for patch in range(len(self.FACTORS)):
            _, control = self.run_patch(patch)
            patch_words = dict(control.patch_outputs)
            for u, v in self.COORDS:
                w = 1.0 - u - v
                evaluation = dr.stage(evaluation_program, self.binding, attributes(patch_words), CONTROL_SYSTEM)
                # the tessellator's coordinate: a per-vertex output of the lane (NAK from_nir.rs load_tess_coord)
                evaluation.outputs[(0, ATTR_TESS_COORD)] = dr.f32(u)
                evaluation.outputs[(0, ATTR_TESS_COORD + 4)] = dr.f32(v)
                dr.execute(evaluation_program, evaluation)
                points = self.POINTS[3 * patch : 3 * patch + 3]
                expected = [points[0][c] * u + points[1][c] * v + points[2][c] * w for c in range(4)]
                self.assertEqual(outputs(evaluation, ATTR_POSITION, 4), expected, f'patch {patch} at {u}, {v}')
                self.assertEqual(outputs(evaluation, ATTR_GENERIC, 4), [u, v, w, float(patch)])


class Tessellation64(Tessellation):
    """The same patches under maxTessellationFactor 64, the device's (GM20B's) maximum: 40 is kept, 70 is 64."""

    NAME, TOP = 'tessellation64', 64.0


class Amplification(Records):
    """A state of maxVertexAmplificationCount 2.

    Each amplification's draw writes its id into the record's AmplificationId word, and the vertex function reads it as
    [[amplification_id]]; it writes its mapping's offsets and the pass's layer and viewport counts into the record's
    words, and the vertex program stores Layer and ViewportIndex from them: the offset below the count, else 0.
    """

    POINTS = ((1.0, 2.0, 3.0, 1.0), (-4.0, 0.5, 0.25, 1.0), (8.0, -16.0, 0.0, 2.0))

    NAME, COUNT = 'amplification', 2
    # (layer offset, layers, viewport offset, viewports) of each amplification tested: amplification 1's layer past the
    # pass's length renders layer 0
    MAPPINGS = {0: (1, 2, 3, 4), 1: (5, 2, 1, 4)}

    def test_each_amplifications_vertices(self):
        record = self.state(self.NAME)
        amplification_max, id_word, count_word = record['amplification']
        layer_word, viewport_word = record['amplification_offsets']
        length_word, count_word_of_viewports = record['pass_words']
        self.assertEqual(amplification_max, self.COUNT)
        self.assertGreater(id_word, 0)
        self.assertEqual(count_word, 0)
        for word in (layer_word, viewport_word, length_word, count_word_of_viewports):
            self.assertGreater(word, 0)
        program = record['programs'][0]
        mappings = self.MAPPINGS
        for amplification in mappings:
            binding = dr.Binding()
            binding.bind((1, 0), dr.floats(*[c for p in self.POINTS for c in p]))
            push = dr.record_push(record)
            push[id_word - 1] = amplification
            layer, layers, viewport, viewports = mappings[amplification]
            push[layer_word - 1], push[length_word - 1] = layer, layers
            push[viewport_word - 1], push[count_word_of_viewports - 1] = viewport, viewports
            binding.tables(dr.root_table(push, record['set_bytes']), dr.render_set(record, binding))
            for vid, point in enumerate(self.POINTS):
                words = {ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}
                environment = dr.stage(program, binding, attributes(words), {})
                dr.execute(program, environment)
                expected = [point[0] + 0.5 * amplification, *point[1:]]
                self.assertEqual(outputs(environment, ATTR_POSITION, 4), expected, f'{amplification} {vid}')
                self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [float(amplification), float(vid), 0.0, 1.0])
                self.assertEqual(environment.outputs[(0, ATTR_LAYER)], layer if layer < layers else 0)
                self.assertEqual(environment.outputs[(0, ATTR_VIEWPORT)], viewport if viewport < viewports else 0)


class Amplification8(Amplification):
    """maxVertexAmplificationCount 8, every count the amplify macro keeps a mapping word for (SWITCH_GM20B_AMP_MAX)."""

    NAME, COUNT = 'amplification8', 8
    MAPPINGS = {0: (1, 8, 3, 16), 5: (6, 8, 15, 16), 7: (7, 8, 16, 16)}


class LayeredFetch(Records):
    """A slice-choosing state of m2v's layered storage form, whose fragment function fetches [[color(0)]].

    The vertex program stores instance i as Layer below the pass's length word (an index at or past it as 0). The
    fragment program, run on quads of each layer of passes of 2 and 6 layers, reads its Layer input and loads and stores
    the attachment's storage image, a 2D array (SULD and SUST of three coordinates), at (x, y, Layer): each layer's own
    texel with its channels rotated, as the Metal source computes it on the CPU, compared as the RGBA8 codes stored.
    """

    HEADER = 0x123  # the attachment view's header index set 0 holds (raw_gpu.cpp: tic | tsc << 20, no sampler)
    QUADS = ((2, 4), (13, 7))

    @staticmethod
    def codes(x: int, y: int, layer: int) -> list[int]:
        """Give the RGBA8 codes of the storage image's texel (x, y) of `layer`, each layer's its own."""
        return [(7 * x + 31 * layer) & 255, (5 * y + 47 * layer) & 255, (x + y + 90 * layer) & 255, 200 + layer]

    @classmethod
    def texel(cls, x: int, y: int, layer: int) -> list[int]:
        """Give the texel as SULD.P loads an RGBA8Unorm one: each code's value."""
        return [dr.f32(c / 255) for c in cls.codes(x, y, layer)]

    def setUp(self) -> None:
        self.record = self.state('layered_fetch')
        storage_mask, _writes, _view = self.record['storage']
        self.assertEqual(storage_mask, 1, 'the storage form')
        self.length_word = self.record['pass_words'][0]
        self.assertTrue(0 < self.length_word <= 32, 'the layered form names its length word')

    def binding(self, layers: int) -> dr.Binding:
        binding = dr.Binding()
        push = dr.record_push(self.record)
        push[self.length_word - 1] = layers
        set0 = dr.render_set(self.record, binding, {0: self.HEADER})
        binding.tables(dr.root_table(push, self.record['set_bytes']), set0)
        return binding

    def layer_of(self, binding: dr.Binding, instance: int) -> int:
        program = self.record['programs'][0]
        environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: 0, ATTR_INSTANCE_ID: instance}), {})
        dr.execute(program, environment)
        return environment.outputs[(0, ATTR_LAYER)]

    def lane(self, banks: dr.Stage, pixel: tuple[int, int], layer: int, loads: list) -> Inputs:
        x, y = pixel
        words = {ATTR_LAYER: layer, ATTR_POSITION: dr.f32(x + 0.5), ATTR_POSITION + 4: dr.f32(y + 0.5)}
        words.update({ATTR_POSITION + 8: dr.f32(0.0), ATTR_POSITION + 12: dr.f32(1.0)})

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        def surface(op: tuple) -> list[int]:
            if op[0] != 'suld':
                raise UnsupportedError(f'{op[0]}, a read the test gave no memory for')
            loads.append((op[1], op[2]))
            return self.texel(*op[2]) if len(op[2]) == 3 else [0, 0, 0, 0]

        return Inputs(attribute, banks.cbuf, surface)

    def test_the_vertex_program_writes_each_instances_layer_below_the_length(self):
        for layers in (2, 6):
            binding = self.binding(layers)
            for instance in range(layers + 1):
                want = instance if instance < layers else 0
                self.assertEqual(self.layer_of(binding, instance), want, f'{layers} layers, instance {instance}')

    def test_each_layers_fragments_fetch_and_store_their_own_texel(self):
        program = self.record['programs'][1]
        for layers in (2, 6):
            binding = self.binding(layers)
            banks = dr.stage(program, binding, attributes({}), {})
            for instance in range(layers):
                layer = self.layer_of(binding, instance)
                for x0, y0 in self.QUADS:
                    pixels = [(x0 + k % 2, y0 + k // 2) for k in range(4)]
                    loads: list[list] = [[] for _ in pixels]
                    lanes = [self.lane(banks, p, layer, loads[k]) for k, p in enumerate(pixels)]
                    for (x, y), loaded, (_, killed, stores) in zip(
                        pixels, loads, run_quad(dr.code(program), lanes, 0), strict=True
                    ):
                        at = (x, y, layer)
                        self.assertFalse(killed, f'{layers} layers: {at}')
                        self.assertEqual(loaded, [(self.HEADER, at)], f'{layers} layers: the loads at {at}')
                        self.assertEqual([s[:2] for s in stores], [(self.HEADER, at)], f'{layers} layers: {at}')
                        stored = [round(dr.value(w) * 255) for w in stores[0][2]]
                        r, g, b, a = self.codes(*at)
                        self.assertEqual(stored, [g, b, r, a], f'{layers} layers: the codes stored at {at}')


class WideStride(Records):
    """Layout strides past SET_VERTEX_STREAM_A_FORMAT's 12 bits.

    A static layout of 2052 and one of 4100 bytes are pulled in the shader, the tint's 16-byte layout is the record's
    one fixed-function stream at location 0, and a dynamic layout's stride twin pulls it at its bind's stride from the
    twin's VertexBufferStride word.
    """

    VERTICES = 4

    @staticmethod
    def laid(values: list[tuple], stride: int) -> bytes:
        out = bytearray(stride * len(values))
        for k, v in enumerate(values):
            struct.pack_into(f'<{len(v)}f', out, k * stride, *v)
        return bytes(out)

    @staticmethod
    def blob(record: dict) -> dict[int, int]:
        words = record['blob']
        return dict(zip(words[0::2], words[1::2], strict=True))

    def test_a_static_stride_past_12_bits_is_pulled_and_the_fixed_function_attribute_keeps_location_0(self):
        record = self.state('wide_static')
        self.assertEqual(record['vertex_slot'], 3)
        self.assertEqual(record['vertex_stride'], 16)
        self.assertEqual(record['vertex_streams'], [0] * 7)
        registers = self.blob(record)
        self.assertEqual(registers.get(0x1C00, 0) & 0xFFF, 16)
        self.assertEqual(registers.get(0x1160, 0) & 0x1F, 0)
        self.assertEqual(registers.get(0x1164, 0) & (1 << 6), 1 << 6)
        positions = [(float(k), float(-2 * k)) for k in range(self.VERTICES)]
        colours = [(0.5 * k, 1.0, 2.0 * k, 4.0) for k in range(self.VERTICES)]
        tints = [(2.0, 0.5, 1.0, float(k + 1)) for k in range(self.VERTICES)]
        binding = dr.Binding()
        binding.bind((1, 1), self.laid(positions, 2052))
        binding.bind((1, 2), self.laid(colours, 4100))
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        program = record['programs'][0]
        for vid in range(self.VERTICES):
            words = {ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}
            words.update({ATTR_GENERIC + 4 * c: dr.f32(tints[vid][c]) for c in range(4)})
            environment = dr.stage(program, binding, attributes(words), {})
            dr.execute(program, environment)
            self.assertEqual(outputs(environment, ATTR_POSITION, 4), [*positions[vid], 0.0, 1.0])
            self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [colours[vid][c] * tints[vid][c] for c in range(4)])

    def test_a_dynamic_strides_twin_pulls_the_layout_at_its_bind_stride(self):
        record = self.state('wide_dynamic')
        self.assertEqual(record['vertex_stride_dynamic'], 1)
        self.assertEqual(record['vertex_slot'], 1)
        self.assertIn('stride_twin', record)
        twin = record['stride_twin']
        self.assertEqual(twin['vertex_stride_dynamic'], 0)
        self.assertEqual(twin['vertex_slot'], 0xFF)
        pulled = [p for p in twin['pulled_stride'] if p[1]]
        self.assertEqual([p[0] for p in pulled], [1])
        program = twin['programs'][0]
        for stride in (4112, 8192 + 4):
            positions = [(float(3 * k + 1), float(k) * 0.25) for k in range(self.VERTICES)]
            binding = dr.Binding()
            binding.bind((1, 1), self.laid(positions, stride))
            binding.bind((1, 2), dr.floats(0.25, 0.5, 0.75, 1.0))
            push = dr.record_push(twin)
            push[pulled[0][1] - 1] = stride
            binding.tables(dr.root_table(push, twin['set_bytes']), dr.render_set(twin, binding))
            for vid in range(self.VERTICES):
                environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}), {})
                dr.execute(program, environment)
                self.assertEqual(outputs(environment, ATTR_POSITION, 4), [*positions[vid], 0.0, 1.0], f'{stride}')
                self.assertEqual(outputs(environment, ATTR_GENERIC, 4), [0.25, 0.5, 0.75, 1.0])


class Interpolants(Records):
    """32 float4 varyings, GM20B's 128 interpolated components: the vertex program stores each at its generic
    attribute, and the fragment program interpolates all of them."""

    COUNT = 32

    @classmethod
    def varying(cls, i: int, vid: int) -> list[float]:
        return [float(i), float(vid), 0.5 * i, 1.0 + i]

    def test_the_vertex_program_writes_every_generic_attribute(self):
        record = self.state('interpolants')
        program = record['programs'][0]
        binding = dr.Binding()
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        for vid in (0, 3):
            environment = dr.stage(program, binding, attributes({ATTR_VERTEX_ID: vid, ATTR_INSTANCE_ID: 0}), {})
            dr.execute(program, environment)
            for i in range(self.COUNT):
                self.assertEqual(outputs(environment, ATTR_GENERIC + 16 * i, 4), self.varying(i, vid), f'{vid} v{i}')

    def test_the_fragment_program_reads_every_generic_attribute(self):
        record = self.state('interpolants')
        program = record['programs'][1]
        binding = dr.Binding()
        binding.tables(dr.root_table(dr.record_push(record), record['set_bytes']), dr.render_set(record, binding))
        banks = dr.stage(program, binding, attributes({}), {})
        vid = 3
        words = {ATTR_POSITION + 4 * c: dr.f32(v) for c, v in enumerate((0.5, 0.5, 0.0, 1.0))}
        for i in range(self.COUNT):
            for c, v in enumerate(self.varying(i, vid)):
                words[ATTR_GENERIC + 16 * i + 4 * c] = dr.f32(v)

        def attribute(address: int) -> int:
            if address not in words:
                raise UnsupportedError(f'IPA {address:#x}, an input the test gave no word for')
            return words[address]

        lanes = [Inputs(attribute, banks.cbuf, lambda key: [0, 0, 0, 0]) for _ in range(4)]
        want = [sum(self.varying(i, vid)[c] * (i + 1) for i in range(self.COUNT)) for c in range(4)]
        for colour, killed, _ in run_quad(dr.code(program), lanes, 4):
            self.assertFalse(killed)
            self.assertEqual([dr.value(w) for w in colour], want)


# QMDV01_07's SHARED_MEMORY_SIZE MW(561:544) and L1_CONFIGURATION MW(671:669), whose 3 is 48 KiB directly addressable
# (clb1c0qmd.h)
QMD_SHARED_MEMORY_SIZE, QMD_L1_CONFIGURATION, QMD_L1_48KB = (544, 18), (669, 3), 3


def qmd_field(qmd: list[int], field: tuple[int, int]) -> int:
    first, width = field
    return sum((qmd[(first + b) // 32] >> ((first + b) % 32) & 1) << b for b in range(width))


class ThreadgroupMemory(Records):
    """A kernel of 48 KiB of threadgroup memory, every word of it used, under the QMD's own shared memory size."""

    THREADS = 256

    def test_a_kernel_of_48_kib_takes_the_whole_cta(self):
        record = self.kernel('wide_threadgroup')
        size = qmd_field(record['qmd'], QMD_SHARED_MEMORY_SIZE)
        self.assertEqual(size, 48 << 10)
        self.assertEqual(qmd_field(record['qmd'], QMD_L1_CONFIGURATION), QMD_L1_48KB)
        binding = dr.Binding()
        out = binding.bind((3, 0), bytes(4 * self.THREADS))
        push = dr.record_push(record)
        if record['region_word'] >= 0:
            for k, v in enumerate([0, 0, 0, self.THREADS - 1, self.THREADS, 1, 1, 0]):
                push[record['region_word'] + k] = v
        binding.tables(dr.root_table(push, record['set_bytes'], {12: 1, 16: 1, 20: 1}), dr.kernel_set(record, binding))
        shared = bytearray(size)
        for tid in range(self.THREADS):
            system = {SV_TID_X: tid, 0x22: 0, 0x23: 0, SV_COMBINED_TID: tid, SV_LANE_ID: tid % 32}
            system.update({SV_CTAID_X: 0, 0x26: 0, 0x27: 0})
            environment = dr.stage(record, binding, attributes({}), system)
            environment.shared = shared
            dr.execute(record, environment, header=False)
        got = struct.unpack(f'<{self.THREADS}I', binding.memory.read(out, 4 * self.THREADS))
        want = []
        for tid in range(self.THREADS):
            x = 0
            for k in range(48):
                x ^= ((tid + 256 * k) * 2654435761 + k) & 0xFFFF_FFFF
            want.append(x)
        self.assertEqual(list(got), want)

    def test_a_kernel_past_48_kib_is_refused_naming_the_limit(self):
        self.assertNotIn('past_threadgroup', RECORDS['kernels'])
        why = RECORDS['refused'].get('past_threadgroup', '')
        self.assertIn('MaxComputeSharedMemorySize', why)
        self.assertIn('available: 49152', why)


class Kernels(Records):
    MASK = 0xFFFF_FFFF_FFFF_FFFF
    ERROR = MASK  # raw_counters.h kErrorValue
    # nanoseconds a PTIMER unit: 32 units a 19.2 MHz tick
    NUM, DEN = 625, 384

    @staticmethod
    def dispatch(record: dict, binding: dr.Binding, groups: int, local: int, stage_in: list[int] | None = None) -> None:
        """Run every thread of a dispatchThreadgroups: of `groups` threadgroups of `local` threads on x.

        The root table's group count (nvk_root_descriptor_table cs.group_count) and the record's DispatchRegion words as
        raw_compute.cpp writes them (raw_dispatch_regions.h computeDispatch, computeRegionWords: base 0, the packed
        threadgroup size less 1, the grid's threads); each thread's TID, CTAID and lane in its warp of 32.
        """
        push = dr.record_push(record)
        if stage_in is not None:
            word = record['stage_in_region_word']
            for k, v in enumerate(stage_in):
                push[word - 1 + k] = v
        if record['region_word'] >= 0:
            for k, v in enumerate([0, 0, 0, local - 1, groups * local, 1, 1, 0]):
                push[record['region_word'] + k] = v
        root = dr.root_table(push, record['set_bytes'], {12: groups, 16: 1, 20: 1})
        binding.tables(root, dr.kernel_set(record, binding))
        for group in range(groups):
            for tid in range(local):
                system = {SV_TID_X: tid, 0x22: 0, 0x23: 0, SV_COMBINED_TID: tid, SV_LANE_ID: tid % 32}
                system.update({SV_CTAID_X: group, 0x26: 0, 0x27: 0})
                dr.execute(record, dr.stage(record, binding, attributes({}), system), header=False)

    def test_a_stage_in_kernel_reads_its_region_from_its_origin(self):
        record = self.kernel('stage_in')
        self.assertGreater(record['stage_in_region_word'], 0)
        origin, size, stride = 3, 8, 12
        values = [(float(k), 0.5 * k - 1.0) for k in range(origin + size)]
        source = bytearray(stride * len(values))
        for k, v in enumerate(values):
            struct.pack_into('<2f', source, k * stride, *v)
        binding = dr.Binding()
        binding.bind((3, 0), bytes(source))
        out = binding.bind((3, 1), bytes(4 * size))
        self.dispatch(record, binding, 1, size, [origin, 0, 0, 0, size, 1, 1, 0])
        written = struct.unpack(f'<{size}f', binding.memory.read(out, 4 * size))
        expected = [values[origin + g][0] * 2.0 + values[origin + g][1] for g in range(size)]
        self.assertEqual(list(written), expected)

    def resolve(self, records: list[tuple[int, int]], published: int, slots: dict[int, tuple[int, int]]) -> list[int]:
        """Give the counter resolve kernel's samples of `records`, each (payload, units).

        The kernel runs beside a clock anchor table of `published` anchors, `slots` its (units, ns) by slot, bound as
        RawMetalCounterEncode.mm binds them.
        """
        layout = RECORDS['layouts']['counter']
        self.assertEqual(layout['sample_bytes'], 8, 'a sample is a 64-bit time')
        record = self.kernel('counter_resolve')
        count = len(records)
        binding = dr.Binding()
        binding.bind((3, 0), dr.counter_records(layout, records))
        out = binding.bind((3, 1), bytes(8 * count))
        binding.bind((3, 2), dr.resolve_params(layout, count))
        binding.bind((3, 3), dr.clock_anchors(layout, published, slots))
        self.dispatch(record, binding, (count + 63) // 64, 64)
        return list(struct.unpack(f'<{count}Q', binding.memory.read(out, 8 * count)))

    def test_the_counter_resolve_kernel_gives_nanoseconds(self):
        """A sample at or past the table's one anchor resolves with it, and one before it to the error value."""
        anchor_units, anchor_ns = 0x0000_0312_FFFF_FF00, 0x0000_0003_FFFF_FFF0
        edges = [0, 5, 383, 384, 0x1_0000_0005, 0xFFFF_FFFF, -1, -384, -0x2_0000_0001, 0x7FFF_0000_0000]
        spans = edges + [(k * 0x1_3579_BDF1 * (-1) ** k) % (1 << 40) - (1 << 39) for k in range(70)]
        self.assertTrue(all(anchor_units + s >= 0 for s in spans), 'no span wraps below 0')
        records = [(1, anchor_units + s) for s in spans] + [(0, 0x1234)]
        self.assertGreater(len(records), 64)
        got = self.resolve(records, 1, {0: (anchor_units, anchor_ns)})
        expected = [(anchor_ns + s * self.NUM // self.DEN) & self.MASK if s >= 0 else self.ERROR for s in spans]
        self.assertEqual(got, [*expected, 0])

    def test_the_counter_resolve_kernel_of_no_anchor_gives_the_error_value(self):
        self.assertEqual(self.resolve([(1, 77), (0, 77)], 0, {}), [self.ERROR, 0])

    def test_the_counter_resolve_kernel_takes_each_samples_newest_kept_anchor(self):
        """A table of 65 anchors past its ring's slots: anchor n at slot n % slots, the newest slots - 1 kept (3 to 64).

        A sample past anchor n and below anchor n + 1 resolves with anchor n; one of anchor 2's, whose slot the next
        power-up writes, or before it, to the error value.
        """
        slots = RECORDS['layouts']['counter']['anchors']['slots']
        published, step = 65, 0x3_0000_0007
        units = {n: 0xFFFF_FF00 + n * step for n in range(published)}
        ns = {n: 0x3_FFFF_FFF0 + n * 0x2_8000_0011 for n in range(published)}
        table = {n % slots: (units[n], ns[n]) for n in range(published - slots, published)}
        kept = range(published - (slots - 1), published)
        samples = [(n, n * 0x9E37_79B9 % step) for n in kept] + [(kept[0], 0), (kept[-1], 0), (40, step - 1), (63, 384)]
        records = [(1, units[n] + offset) for n, offset in samples]
        records += [(1, units[2] + 5), (1, units[3] - 1), (1, units[2] - 1), (0, units[10])]
        self.assertGreater(len(records), 64)
        expected = [(ns[n] + offset * self.NUM // self.DEN) & self.MASK for n, offset in samples]
        self.assertEqual(self.resolve(records, published, table), [*expected, self.ERROR, self.ERROR, self.ERROR, 0])


if __name__ == '__main__':
    RECORDS.update(load(Path(sys.argv[1]), Path(sys.argv[2])))
    unittest.main(argv=sys.argv[:1])
