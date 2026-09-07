# flash_qio.py — PlatformIO extra script.
#
# The espressif32 builder (builder/main.py:_get_board_flash_mode) deliberately
# downgrades qio/qout -> dio for the Arduino framework, because the prebuilt
# Arduino bootloader is normally DIO. Problem: ESP32-WROVER PSRAM lives on the
# quad-SPI bus and a DIO bootloader can NOT bring it up, so psramInit() fails
# and we lose ~4-8 MB of external RAM.
#
# This script overrides the flash-mode resolver so BOTH the bootloader and the
# application image are emitted as QIO, letting the bootloader initialize
# PSRAM. It only engages when board_build.flash_mode = qio in platformio.ini.

print("[flash_qio] SCRIPT LOADED")

try:
    Import("env")
    print("[flash_qio] env imported")

    # board_build.flash_mode ends up as a project option; read it from there.
    requested = env.GetProjectOption("board_build.flash_mode", "").lower()
    print("[flash_qio] board_build.flash_mode =", repr(requested))

    # Fall back to inspecting the board's build.flash_mode if needed.
    if not requested:
        try:
            requested = env.BoardConfig().get("build.flash_mode", "").lower()
            print("[flash_qio] build.flash_mode =", repr(requested))
        except Exception as e:
            print("[flash_qio] BoardConfig read failed:", repr(e))

    if requested in ("qio", "qout"):
        env.Replace(__get_board_flash_mode=lambda e: requested)
        env.Replace(BOARD_FLASH_MODE=requested)
        print("[flash_qio] forcing flash mode:", requested)
    else:
        print("[flash_qio] flash mode not qio/qout; leaving default")
except Exception as e:
    print("[flash_qio] ERROR:", repr(e))
