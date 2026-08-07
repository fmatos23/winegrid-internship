#!/usr/bin/env python3
"""Origem: escrito de raiz nesta colaboração (Claude), não copiado de nenhum
repositório (usa a biblioteca de terceiros `aiocoap` para o protocolo CoAP,
essa sim uma dependência externa, não código copiado para aqui).

Push a firmware image to the LwM2M client's Object 5 (Firmware Update),
resource /5/0/0 (Package), via a direct block-wise CoAP PUT - the same
mechanism Leshan itself uses under the hood, without going through the
REST-to-CoAP translation layer or a browser.

Usage: python3 lwm2m_ota_push.py <device_ip>:<device_port> <image_path>
(get <device_ip>:<device_port> from `curl -s localhost:8080/api/clients`,
field "address" - it's the client's registration socket, not port 5683).
"""
import asyncio
import sys

from aiocoap import Context, Message, PUT

DEVICE_ADDR = sys.argv[1]
IMAGE_PATH = sys.argv[2]


async def main():
    with open(IMAGE_PATH, "rb") as f:
        payload = f.read()

    print(f"A enviar {len(payload)} bytes para coap://{DEVICE_ADDR}/5/0/0 ...")

    ctx = await Context.create_client_context()
    request = Message(
        code=PUT,
        payload=payload,
        uri=f"coap://{DEVICE_ADDR}/5/0/0",
        content_format=42,  # application/octet-stream
    )
    try:
        response = await asyncio.wait_for(ctx.request(request).response, timeout=180)
        print("Resposta:", response.code, response.payload)
    except asyncio.TimeoutError:
        print("TIMEOUT - sem resposta do dispositivo (pode ter crashado ou perdido a ligacao)")
    except Exception as e:
        print("ERRO:", repr(e))


asyncio.run(main())