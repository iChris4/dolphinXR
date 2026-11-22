#!/usr/bin/env python3
# Reaches the Agent Server (MCP) of DolphinXR on an Apple Vision Pro from this Mac.
#
# On the headset the server listens on 127.0.0.1:<port> only, and the app relays <port + 1> to it
# when [VR] AgentServer and AgentServerToken are set in its GFX.ini. This script listens on this
# Mac's 127.0.0.1:<port> and forwards to <headset>:<port + 1>, so an MCP client uses
# http://127.0.0.1:<port>/mcp with the "Authorization: Bearer <token>" header, as it would with a
# local DolphinXR. (The server accepts loopback Host headers only; relaying keeps them.)
#
# Usage: Source/visionOS/agent-relay.py [--device HOST] [--port 7311]
#   --device  the headset: its CoreDevice tunnel address (default: read from
#             `xcrun devicectl device info details`), or its address on the local network.
import argparse
import asyncio
import re
import subprocess


def tunnel_address(device: str | None) -> str:
    command = ["xcrun", "devicectl", "device", "info", "details"]
    if device:
        command += ["--device", device]
    output = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    match = re.search(r"Tunnel IP Address:\s*(\S+)", output)
    if not match:
        raise SystemExit("No CoreDevice tunnel to the headset: is it connected and paired?")
    return match.group(1)


async def pipe(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
    try:
        while data := await reader.read(65536):
            writer.write(data)
            await writer.drain()
    except ConnectionError:
        pass
    finally:
        writer.close()


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="headset address (default: its CoreDevice tunnel)")
    parser.add_argument("--udid", help="devicectl device identifier, when several are paired")
    parser.add_argument("--port", type=int, default=7311, help="the Agent Server port")
    args = parser.parse_args()
    target = {"host": args.device or tunnel_address(args.udid)}

    async def relay(client_reader, client_writer):
        host = target["host"]
        try:
            server_reader, server_writer = await asyncio.open_connection(host, args.port + 1)
        except OSError as error:
            # The CoreDevice tunnel address changes when the headset reconnects (often over Wi-Fi):
            # look it up again and retry once.
            fresh = None
            if not args.device:
                try:
                    fresh = await asyncio.to_thread(tunnel_address, args.udid)
                except (SystemExit, subprocess.SubprocessError):
                    fresh = None
            if not fresh or fresh == host:
                print(f"cannot reach {host}:{args.port + 1}: {error}", flush=True)
                client_writer.close()
                return
            target["host"] = host = fresh
            print(f"tunnel moved: now [{host}]:{args.port + 1}", flush=True)
            try:
                server_reader, server_writer = await asyncio.open_connection(host, args.port + 1)
            except OSError as retry_error:
                print(f"cannot reach {host}:{args.port + 1}: {retry_error}", flush=True)
                client_writer.close()
                return
        await asyncio.gather(pipe(client_reader, server_writer), pipe(server_reader, client_writer))

    server = await asyncio.start_server(relay, "127.0.0.1", args.port)
    print(f"http://127.0.0.1:{args.port}/mcp -> [{target['host']}]:{args.port + 1}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
