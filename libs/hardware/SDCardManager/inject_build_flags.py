# SDCardManager build hook: force two SdFat options on for the WHOLE build.
# SdFat compiles as its own library, so a define in our library.json "flags"
# would not reach it — it has to be appended to every lib builder's env here.
#
# 1. USE_UTF8_LONG_NAMES, always. Without it, SdFat returns mangled names for
#    any file with a non-ASCII character ("The 71/2 Deaths..." listed but
#    unopenable — no metadata, no cover, no reading). There is no situation
#    where a FreeInk firmware wants that, so this is not a user-facing option.
#
# 2. USE_BLOCK_DEVICE_INTERFACE, but only when the build enables USB Mass
#    Storage (FREEINK_CAP_USB_MSC). It is what makes SdSpiCard derive from
#    FsBlockDeviceInterface, which is how SDCardManager hands the SPI-attached
#    card to a USB host as a raw block device — without it SdSpiCard is a plain
#    concrete class and detachFilesystemForRawAccess() has nothing to return.
#    The SDMMC backend needs the same option for its FsVolume, so coupling both
#    to the capability keeps it off the boards that pay for it in vtables and
#    indirect calls for nothing.
Import("env")

_ALWAYS = [("USE_UTF8_LONG_NAMES", "1")]
_IF_USB_MSC = [("USE_BLOCK_DEVICE_INTERFACE", "1")]


def _defines(e):
    return {d[0] if isinstance(d, (tuple, list)) else d for d in e.get("CPPDEFINES", [])}


def _usb_msc_enabled(e):
    for d in e.get("CPPDEFINES", []):
        if isinstance(d, (tuple, list)) and d[0] == "FREEINK_CAP_USB_MSC":
            return str(d[1]) not in ("0", "")
    return False


def _append(e, wanted):
    have = _defines(e)
    missing = [d for d in wanted if d[0] not in have]
    if missing:
        e.Append(CPPDEFINES=missing)


_wanted = list(_ALWAYS)
# Read the capability off the project env: a lib builder's env may not carry it.
if _usb_msc_enabled(env) or _usb_msc_enabled(DefaultEnvironment()):
    _wanted += _IF_USB_MSC

_append(env, _wanted)
_append(DefaultEnvironment(), _wanted)
for lb in env.GetLibBuilders():
    _append(lb.env, _wanted)
