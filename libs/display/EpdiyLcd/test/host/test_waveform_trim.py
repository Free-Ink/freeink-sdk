"""Byte-exact E0470 regression: golden outputs from the original 256-sequence implementation."""
import base64
import ctypes as c
from pathlib import Path
import re
import subprocess
import tempfile
import zlib

LCD = Path(__file__).resolve().parents[2]


class Phases(c.Structure):
    _fields_ = [('phases', c.c_int), ('luts', c.POINTER(c.c_uint8)), ('phase_times', c.POINTER(c.c_int))]


class Trim(c.Structure):
    _fields_ = [(name, c.c_int) for name in ('erase_max', 'sat_cut', 'white_sat_cut', 'hold')]


# Filled from the unmodified implementation; compressed bytes, not a second trim algorithm.
GOLDEN = {'gc16': (36, 'c%04E%MF7t5Cl*-P=fC%D(;6OQh*N}h^veVo@BK0XV)B(l`JdLKFtUzFim3<V_Zkgai<doe8~51^~lWk;dg5`T+inW&a3%_*iG~dWL9aVr#)%+Qg+2l*Yl1#SNCcsQBR%K?(1jY=-fJ+Pm}6<i#-hPx1Z17M!$W$(|cc-#jE_5&a>oQ#nZ~Y<S_9ZxJRw0#`m%m-aPOl7Jk(J!ec-GXXBN8i|1UnmVA0o#g@kove@=K8}ux7?&V`zPI`VurJkC6t&x7jVY(mAKc5#EWp{1'), 'gl16': (37, 'c%0SMK?=hl5Cu>J%?+A;4`ntzO%BlmWRZgPlvS9KUpwk(Htm261RsAQm>4rw-urFT#2(f@;CJe-*K;QI=v{bx>B(l-@|1yj8h^{?+`(+QC4YrXSFldXCT}yJyM~`E%v{`8TE;9EW>&l8r`1+wuB~GYW#;5nE*kFO)w$%4<QKhPg_`fWcMa<$`K91ti<KN#<{qUvHNKUl@ZELBvLCho=)-;=JKcGI^6b<ebHR4y!*|ZtJn!Kwv@MT@=M-jc=5tltm&Y>_=9KICdFNC{$RBXY>;AsKTW?@4kKF')}


def load_trim(directory, source=None):
    so = Path(directory) / 'trim.so'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC',
                    '-I' + str(LCD / 'src/e0470/include'), '-I' + str(LCD / 'src/epdiy/include'),
                    str(source or LCD / 'src/e0470/e0470_waveform_trim.c'), '-o', str(so)], check=True)
    library = c.CDLL(str(so))
    fn = library.e0470_waveform_trim
    fn.argtypes = [c.POINTER(Phases), c.POINTER(Trim), c.POINTER(c.c_uint8)]
    fn.restype = c.c_int
    return fn


def source_bytes(name):
    text = (LCD / f'src/e0470/waveforms/{name}.h').read_text()
    initializer = text.split('= {', 1)[1].split('};', 1)[0]
    return bytes(int(v, 16) for v in re.findall(r'0x[0-9a-fA-F]+', initializer)).ljust(48 * 64, b'\0')


def run():
    assert set(GOLDEN) == {'gc16', 'gl16'}
    with tempfile.TemporaryDirectory() as directory:
        trim = load_trim(directory)
        config = Trim(11, 5, 0, 3)
        output = (c.c_uint8 * (64 * 64))()
        for name, (expected_phases, golden) in GOLDEN.items():
            data = (c.c_uint8 * (48 * 64)).from_buffer_copy(source_bytes(name))
            src = Phases(48, data, None)
            output[:] = bytes([0xA5]) * len(output)
            assert trim(c.byref(src), c.byref(config), output) == expected_phases
            assert bytes(output[:expected_phases * 64]) == zlib.decompress(base64.b85decode(golden)), name
            assert bytes(output[expected_phases * 64:]) == bytes([0xA5]) * (len(output) - expected_phases * 64)

        data = (c.c_uint8 * (64 * 64))()
        src = Phases(64, data, None)
        assert trim(c.byref(src), c.byref(config), output) == 3  # all-hold
        assert bytes(output[:3 * 64]) == bytes(3 * 64)

        # 63 active phases plus one hold: the maximum valid 64-phase result.
        data[:63 * 64] = bytes([0x55]) * (63 * 64)
        config = Trim(64, 0, 0, 1)
        assert trim(c.byref(src), c.byref(config), output) == 64
        assert bytes(output) == bytes(data)
        config.hold = 2  # would exceed source capacity
        invalid = [(c.byref(src), c.byref(config), output), (None, c.byref(config), output),
                   (c.byref(src), None, output), (c.byref(src), c.byref(config), None)]
        for args in invalid:
            output[:] = bytes([0xA5]) * len(output)
            assert trim(*args) == 0
            assert bytes(output) == bytes([0xA5]) * len(output)
        for count in (0, -1, 65):
            src.phases = count
            assert trim(c.byref(src), c.byref(config), output) == 0
        src.phases = 64
        for values in ((-1, 0, 0, 1), (0, -1, 0, 1), (0, 0, -1, 1), (0, 0, 0, 0)):
            assert trim(c.byref(src), c.byref(Trim(*values)), output) == 0
    print('E0470 GC16/GL16 byte equivalence, invalid arguments, all-hold and 64-phase checks passed')


if __name__ == '__main__':
    run()
