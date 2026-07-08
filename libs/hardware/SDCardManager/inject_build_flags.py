# SDCardManager build hook: force SdFat's UTF-8 long-filename support on for
# the WHOLE build (SdFat compiles as its own library, so a define in our
# library.json "flags" would not reach it).
#
# Without USE_UTF8_LONG_NAMES, SdFat returns mangled names for any file with
# a non-ASCII character ("The 7½ Deaths..." listed but unopenable — no
# metadata, no cover, no reading). There is no situation where a FreeInk
# firmware wants that, so this is not a user-facing option.
Import("env")

_DEFINE = ("USE_UTF8_LONG_NAMES", "1")


def _append(e):
    defines = {d[0] if isinstance(d, tuple) else d for d in e.get("CPPDEFINES", [])}
    if _DEFINE[0] not in defines:
        e.Append(CPPDEFINES=[_DEFINE])


_append(env)
_append(DefaultEnvironment())
for lb in env.GetLibBuilders():
    _append(lb.env)
