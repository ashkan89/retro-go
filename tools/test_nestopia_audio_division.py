"""Compare the prepared Nestopia mixer division with the original quotient."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


PORT = Path(__file__).resolve().parents[1] / "retro-core/components/nestopia"
TEST = r'''
#include <cassert>
#include <cstdint>
using qaword = uint64_t;
using dword = uint32_t;
#define NST_FORCE_INLINE inline
struct Apu {
    struct Cycles {
        qaword sampleWeight;
        dword sampleDivisor;
        unsigned sampleDivisorShift;
        qaword DivideSample(qaword) const;
    };
};
'''
RUNNER = r'''
int main() {
    qaword seed = 0x7a9b4c1d9ef01ab1ULL;
    auto random = [&]() {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        return seed ^= seed << 17;
    };
    for (unsigned i = 0; i < 200000; ++i) {
        Apu::Cycles c{};
        c.sampleWeight = 1 + (random() & ((qaword(1) << (1 + i % 52)) - 1));
        while ((c.sampleWeight >> c.sampleDivisorShift) > 0xFFFF)
            ++c.sampleDivisorShift;
        c.sampleDivisor = c.sampleWeight >> c.sampleDivisorShift;
        const qaword q = random() & 0x7fff;
        const qaword exact = c.sampleWeight * (i % 52 < 48 ? q : q & 0x7ff);
        for (qaword sum : {exact ? exact - 1 : 0, exact, exact + 1,
                           random(), UINT64_MAX}) {
            assert(c.DivideSample(sum) == sum / c.sampleWeight);
        }
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=PORT / "upstream")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as scratch:
        root = Path(scratch)
        subprocess.run([sys.executable, str(PORT / "prepare_core.py"),
                        str(args.source / "source/core"), str(root / "core")], check=True)
        apu = (root / "core/NstApu.cpp").read_text()
        divide = apu[apu.index("NST_FORCE_INLINE qaword Apu::Cycles::DivideSample"):
                     apu.index("NST_NO_INLINE Apu::Channel::Sample Apu::GetSample")]
        source = root / "divide.cpp"
        source.write_text("#include <initializer_list>\n" + TEST + divide + RUNNER)
        exe = root / "divide.exe"
        compiler = [args.cxx] + (["c++"] if Path(args.cxx).stem == "zig" else [])
        subprocess.run(compiler + ["-std=c++11", "-O2", "-UNDEBUG", str(source),
                                   "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: one million exact mixer quotients, including rounding boundaries and wide fallback")


if __name__ == "__main__":
    main()
