"""Invia un'immagine a un tag ESL NEMR 400x300 BWR direttamente dal Mac (bleak).
Uso: esl_send.py <NOME_TAG> <immagine | --demo>"""
import asyncio, struct, sys, time
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 300
ROW = W // 8
SIZE = (H + 7) // 8 * W * 2          # 30400, accettato dal tag
CMD = "0000fef1-0000-1000-8000-00805f9b34fb"
IMG = "0000fef2-0000-1000-8000-00805f9b34fb"

def ink(p):
    r, g, b = p[:3]
    if r > 150 and r > g + b:
        return 2
    return 1 if (r + g + b) / 3 < 120 else 0

def pack(img):
    img = img.convert('RGB').resize((W, H))
    px = img.load()
    frame = bytearray(SIZE)
    for y in range(H):
        for x in range(W):
            c = ink(px[x, y])
            o, bit = y * ROW + x // 8, 0x80 >> (x % 8)
            if c == 2:
                frame[ROW * H + o] |= bit
            if c != 1:
                frame[o] |= bit
    return frame

def demo():
    img = Image.new('RGB', (W, H), 'white')
    d = ImageDraw.Draw(img)
    f = lambda s: ImageFont.truetype('/System/Library/Fonts/Helvetica.ttc', s)
    d.rectangle([0, 0, W, 70], fill='red')
    d.text((W / 2, 35), 'DIRETTO DAL MAC', font=f(34), fill='white', anchor='mm')
    d.text((W / 2, 160), time.strftime('%H:%M'), font=f(110), fill='black', anchor='mm')
    d.text((W / 2, 255), 'macOS  ->  BLE  ->  e-paper', font=f(24), fill='black', anchor='mm')
    return img

async def request(client, q, char, data, timeout):
    while not q.empty():
        q.get_nowait()
    await client.write_gatt_char(char, data, response=True)
    return await asyncio.wait_for(q.get(), timeout)

async def main(name, frame):
    dev = await BleakScanner.find_device_by_name(name, timeout=20)
    if not dev:
        sys.exit(f'{name} non trovato')
    async with BleakClient(dev) as client:
        print('connesso, MTU', client.mtu_size)
        q = asyncio.Queue()
        cb = lambda _, d: q.put_nowait(bytes(d))
        await client.start_notify(CMD, cb)
        await client.start_notify(IMG, cb)
        await asyncio.sleep(0.3)        # senza pausa la prima risposta va persa
        r = await request(client, q, CMD, b'\x01', 2)
        block = struct.unpack_from('<H', r, 1)[0]
        print('blockSize', block)
        r = await request(client, q, CMD, b'\x02' + struct.pack('<I', len(frame)) + b'\0\0\0', 2)
        assert r[:2] == b'\x02\x00', r.hex()
        r = await request(client, q, CMD, b'\x03', 2)
        assert r[:2] == b'\x05\x00', r.hex()
        chunk, step, t0 = 0, block - 4, time.time()
        while True:
            data = frame[chunk * step:(chunk + 1) * step]
            r = await request(client, q, IMG, struct.pack('<I', chunk) + data, 10)
            if r[1] == 8:
                break
            chunk = struct.unpack_from('<I', r, 2)[0]
        print(f'inviato in {time.time() - t0:.1f} s')

name, src = sys.argv[1], sys.argv[2]
asyncio.run(main(name, pack(demo() if src == '--demo' else Image.open(src))))
