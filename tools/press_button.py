#!/usr/bin/env python3
"""Press a template button on the device via the ESPHome native API.

Useful to rule out stale Home Assistant entities when a button "does nothing".

    python3 press_button.py <host> <api-key-base64> <button-substring>

Requires: pip install aioesphomeapi
"""
import asyncio
import sys

from aioesphomeapi import APIClient, ButtonInfo


async def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    host, key, needle = sys.argv[1], sys.argv[2], sys.argv[3].lower()
    cli = APIClient(host, 6053, "", noise_psk=key)
    await cli.connect(login=True)
    _, _, ents = await cli.device_info_and_list_entities()
    btns = [e for e in ents if isinstance(e, ButtonInfo)]
    for b in btns:
        print("BTN:", b.object_id, "|", b.name)
    sel = [b for b in btns if needle in b.object_id.lower() or needle in b.name.lower()]
    if not sel:
        sys.exit("no button matches %r" % needle)
    print("PRESSING:", sel[0].object_id)
    cli.button_command(sel[0].key)
    await asyncio.sleep(2)
    await cli.disconnect()


asyncio.run(main())
