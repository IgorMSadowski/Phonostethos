"""Check real worker coordination without a Pi, USB drive or GPIO module.

ALSA's null device replaces the sound hardware. A temporary folder and regular
file replace USB storage and the LED device. This does not test physical sound.
"""
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path


def read_exact(client, length):
    """TCP can split a frame. Keep reading until all requested bytes arrive."""
    data = b""
    while len(data) < length:
        block = client.recv(length - len(data))
        assert block, "Server disconnected before the complete frame arrived"
        data += block
    return data


def read_frame(client):
    frame_type = read_exact(client, 1)[0]
    if frame_type == 2:
        length = 4  # Status is the fixed five-byte exception to normal framing.
    else:
        length = struct.unpack("!I", read_exact(client, 4))[0]
    assert length <= 16384, "Invalid server frame length"
    return frame_type, read_exact(client, length)


def wait_for(client, frame_type, command=None, transport=None):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        received_type, payload = read_frame(client)
        if received_type != frame_type:
            continue
        if command is not None:
            if payload[0] != command or payload[1] == 1:
                continue  # AcceptedPending is not the final command result.
        if transport is not None and payload[1] != transport:
            continue
        return payload
    raise AssertionError("Expected server response did not arrive")


def send_command(client, command, value):
    client.sendall(struct.pack("!BI", command, value))
    return wait_for(client, 5, command=command)


def connect_when_ready(port, server):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            client = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            client.settimeout(5)
            return client
        except OSError:
            assert server.poll() is None, "Server failed during startup"
            time.sleep(0.02)
    raise AssertionError("Server did not start listening")


def check_server(executable, directory):
    wav = directory / "GENERAL_20261123_120000_000001.wav"
    with wave.open(str(wav), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(8000)
        # An empty but valid WAV isolates the EOF control transition. ALSA null
        # consumes audio without wall-clock pacing, so longer files can trigger
        # intentional buffer-underflow handling rather than normal EOF.
        # runtime-test separately checks sample transfer with a nonempty WAV.
        audio.writeframes(b"")
    led = directory / "led-bytes"
    led.touch()
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    with (directory / "server.log").open("w+") as log:
        server = subprocess.Popen([
            executable, "--bind", "127.0.0.1", "--port", str(port),
            "--capture-pcm", "null", "--playback-pcm", "null",
            "--storage", str(directory), "--allow-unmounted-storage",
            "--led-device", str(led),
        ], stdout=log, stderr=log)
        try:
            with connect_when_ready(port, server) as client:
                state = wait_for(client, 4)
                assert state[0] == 1 and state[1] == 0
                for gain in [101, 102, 103, 1, 5, 20]:
                    assert send_command(client, 2, gain)[1] == 0

                # A partial command must be retained until its last bytes arrive.
                packet = struct.pack("!BI", 2, 3)
                client.sendall(packet[:2])
                client.sendall(packet[2:])
                assert wait_for(client, 5, command=2)[1] == 0
                assert send_command(client, 2, 99)[1] == 2  # Invalid gain.

                client.sendall(struct.pack("!BI", 8, 0))
                listing = wait_for(client, 3).decode()
                file_id = int(listing.splitlines()[1].split("\t")[0])
                assert wait_for(client, 5, command=8)[1] == 0
                assert send_command(client, 5, file_id)[1] == 0
                wait_for(client, 4, transport=0)  # EOF cleanup must restore LIVE.

                # This caught missing snd_pcm_prepare() after draining playback:
                # repeated audio errors flooded Main and delayed later replies.
                assert send_command(client, 2, 1)[1] == 0
                assert send_command(client, 5, 0xffffffff)[1] == 2
                assert send_command(client, 9, file_id)[1] == 0
                assert not wav.exists()
        finally:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
                raise AssertionError("Server did not stop its worker threads")
            log.seek(0)
            output = log.read()
            assert server.returncode == 0, output[-4000:]
            assert "File descriptor in bad state" not in output, output[-4000:]


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="phono-controller-test-") as path:
        check_server(sys.argv[1], Path(path))
    print("PASS: gain codes, TCP fragments, playback/EOF/live transition and archive commands")
