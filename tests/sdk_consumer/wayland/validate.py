"""Exercise the external World Demo on isolated Sway outputs and integer-only Weston."""
import argparse
import contextlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


@contextlib.contextmanager
def running(command, env, log):
    with log.open("w") as output, subprocess.Popen(
        command, env=env, stdout=output, stderr=subprocess.STDOUT
    ) as process:
        try:
            yield process
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


def wait_for(check, process, description):
    print(f"Waiting for {description}", flush=True)
    end = time.monotonic() + 90
    while time.monotonic() < end:
        result = check()
        if result:
            return result
        if process.poll() is not None:
            raise RuntimeError(f"{description}: process exited with {process.returncode}")
        time.sleep(0.1)
    raise RuntimeError(f"Timed out waiting for {description}")


def protocol_globals(env, log, fractional):
    print(f"Inspecting compositor globals on {env['WAYLAND_DISPLAY']}", flush=True)
    value = subprocess.check_output(["wayland-info"], env=env, text=True, timeout=20)
    log.write_text(value)
    if ("wp_fractional_scale_manager_v1" in value) != fractional:
        raise RuntimeError(f"Unexpected fractional-scale protocol inventory in {log}")


def presented(trace, width, height):
    # Lavapipe creates wl_shm buffers for the Vulkan swapchain. Match an actual
    # attach and commit of one of those exact buffers, including on output moves.
    buffers = re.finditer(
        rf"create_buffer\(new id (wl_buffer[@#]\d+), \d+, {width}, {height},", trace
    )
    for buffer in buffers:
        later = trace[buffer.end():]
        attach = re.search(r"(wl_surface[@#]\d+)\.attach\(" + re.escape(buffer[1]) + r",", later)
        if attach and attach[1] + ".commit(" in later[attach.end():]:
            return True
    return False


def sway(demo, directory, env):
    config = directory / "sway.conf"
    config.write_text("xwayland disable\nseat seat0 fallback true\n"
                      "default_floating_border none\nfor_window [app_id=\"ludus\"] floating enable\n"
                      "output HEADLESS-1 mode 1280x960 scale 1\n"
                      "output HEADLESS-2 mode 1920x1440 scale 1.5\n"
                      "output HEADLESS-3 mode 2560x1920 scale 2\n")
    runtime = Path(env["XDG_RUNTIME_DIR"])
    with running(["sway", "-c", str(config)], env, directory / "sway.log") as compositor:
        socket = wait_for(lambda: next(runtime.glob("sway-ipc*.sock"), None), compositor, "Sway IPC")
        display = wait_for(lambda: next((p for p in runtime.glob("wayland-*") if p.is_socket()), None),
                           compositor, "Sway display")
        env = dict(env, SWAYSOCK=str(socket), WAYLAND_DISPLAY=display.name)
        protocol_globals(env, directory / "sway-globals.txt", True)

        def command(*args):
            print(f"Sway IPC: {args}", flush=True)
            result = json.loads(subprocess.check_output(["swaymsg", "-r", *args], env=env, timeout=20))
            if any(item.get("success") is False for item in result):
                raise RuntimeError(str(result))
            return result

        (directory / "outputs.json").write_text(json.dumps(command("-t", "get_outputs"), indent=2))
        command("focus output HEADLESS-1")
        log = directory / "world-sway.log"
        with running(["stdbuf", "-oL", str(demo)], dict(env, WAYLAND_DEBUG="client"), log) as game:
            read = lambda: log.read_text(errors="replace")
            wait_for(lambda: presented(read(), 960, 640), game, "initial 960x640 presentation")
            stage = log.stat().st_size
            command('[app_id="ludus"] resize set width 401 px height 301 px')
            for index, (scale, width, height) in enumerate([(1, 401, 301), (1.5, 602, 452), (2, 802, 602)], 1):
                if index > 1:
                    stage = log.stat().st_size
                    command(f'[app_id="ludus"] move container to output HEADLESS-{index}')
                    command(f"focus output HEADLESS-{index}")
                def correct_frame():
                    with log.open("rb") as data:
                        data.seek(stage)
                        trace = data.read().decode(errors="replace")
                    return (f"Wayland logical 401x301, backing {width}x{height}, scale {scale}" in trace
                            and presented(trace, width, height))
                wait_for(correct_frame, game, f"{scale}x backing pixels and Vulkan presentation")
                after = log.stat().st_size
                subprocess.run(["wtype", "-s", "500", "-P", "space", "-s", "150", "-p", "space"], env=env, check=True, timeout=20)
                def input_and_frame():
                    with log.open("rb") as data:
                        data.seek(after)
                        return re.search(rb"wl_keyboard[@#]\d+\.key\([^\n]*, (\d+), 1\).*?wl_keyboard[@#]\d+\.key\([^\n]*, \1, 0\).*?wl_surface[@#]\d+\.commit\(", data.read(), re.S)
                wait_for(input_and_frame, game, f"keyboard delivery and next present at {scale}x")
                print(f"Sway HEADLESS-{index}: logical 401x301, presented {width}x{height}, Space delivered", flush=True)
            command('[app_id="ludus"] kill')
            if game.wait(timeout=15) != 0:
                raise RuntimeError("World Demo failed on Sway")


def weston(demo, directory, env, native_build):
    env = dict(env, WAYLAND_DISPLAY="wayland-integer")
    with running(["weston", "--backend=headless", "--renderer=pixman", "--idle-time=0",
                  "--no-config", "--width=1920", "--height=1440", "--scale=2",
                  "--socket=wayland-integer"], env, directory / "weston.log") as compositor:
        socket = Path(env["XDG_RUNTIME_DIR"]) / env["WAYLAND_DISPLAY"]
        wait_for(socket.is_socket, compositor, "Weston display")
        protocol_globals(env, directory / "weston-globals.txt", False)
        print("Running Platform event tests on Weston", flush=True)
        with (directory / "platform-events.log").open("w") as output:
            subprocess.run([native_build / "modules/platform/ludus_platform_wayland_tests"],
                           env=dict(env, LUDUS_TEST_WAYLAND="1"), stdout=output, stderr=subprocess.STDOUT,
                           check=True, timeout=30)
        rhi_tests = native_build / "modules/graphics/rhi/ludus_graphics_rhi_tests"
        print("Checking zero-size recovery on Weston", flush=True)
        with (directory / "zero-extent.log").open("w") as output:
            subprocess.run([rhi_tests, "[wayland][resize]"], env=dict(env, LUDUS_TEST_WAYLAND="1"),
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=60)
        log = directory / "world-weston.log"
        with running([str(demo), "--frames", "30"], dict(env, WAYLAND_DEBUG="client"), log) as game:
            if game.wait(timeout=90) != 0:
                raise RuntimeError("World Demo failed on Weston")
        trace = log.read_text(errors="replace")
        if "Wayland logical 960x640, backing 1920x1280" not in trace or not presented(trace, 1920, 1280):
            raise RuntimeError("Integer-only compositor did not present a 2x buffer")
        print("Weston integer fallback: logical 960x640, presented 1920x1280", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("demo", type=Path)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--native-build", type=Path, default=Path("out/build/linux-clang-development"))
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as runtime:
        driver, = Path("/usr/share/vulkan/icd.d").glob("lvp_icd*.json")
        environment = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
                           WLR_RENDERER="pixman", WLR_HEADLESS_OUTPUTS="3", VK_DRIVER_FILES=str(driver))
        failures = []
        for compositor, extra in ((sway, ()), (weston, (args.native_build.resolve(),))):
            try:
                compositor(args.demo.resolve(), args.directory.resolve(), environment, *extra)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                failures.append(f"{compositor.__name__}: {error}")
                print(failures[-1], flush=True)
        if failures:
            raise SystemExit("\n".join(failures))
