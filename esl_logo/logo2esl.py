# Converte un'immagine in framebuffer ESL BWR 400x300 (piani per righe, bit7 = sinistra)
import sys
from PIL import Image
W, H = 400, 300
TOP, BOTTOM, SIDE = 6, 22, 8
src, out_h, out_png = sys.argv[1], sys.argv[2], sys.argv[3]
img = Image.open(src).convert('RGB')

def ink(p):
    r, g, b = p
    if r > 150 and r > g + b:
        return 2                      # rosso
    if (r + g + b) / 3 < 120:
        return 1                      # nero (blu scuro)
    return 0

# ritaglio sul contenuto (salta la cornice grigia dello screenshot)
w0, h0 = img.size
px = img.load()
xs, ys = [], []
for y in range(8, h0 - 8, 2):
    for x in range(8, w0 - 8, 2):
        if ink(px[x, y]):
            xs.append(x); ys.append(y)
box = (min(xs) - 4, min(ys) - 4, max(xs) + 6, max(ys) + 6)
img = img.crop(box)
scale = min((W - 2 * SIDE) / img.width, (H - TOP - BOTTOM) / img.height)
img = img.resize((round(img.width * scale), round(img.height * scale)), Image.LANCZOS)
ox, oy = (W - img.width) // 2, TOP + (H - TOP - BOTTOM - img.height) // 2

row = (W + 7) // 8
# il piano rosso inizia subito dopo il B/W (W*H/8); la dimensione inviata
# resta (H+7)/8*W*2, accettata dal tag
plane = row * H
frame = bytearray((H + 7) // 8 * W * 2)
prev = Image.new('RGB', (W, H), (235, 235, 235))
sp = img.load()
for y in range(H):
    for x in range(W):
        c = 0
        if ox <= x < ox + img.width and oy <= y < oy + img.height:
            c = ink(sp[x - ox, y - oy])
        o, bit = y * row + x // 8, 0x80 >> (x % 8)
        if c == 2:
            frame[plane + o] |= bit
        if c != 1:
            frame[o] |= bit
        prev.putpixel((x, y), [(235, 235, 235), (15, 15, 15), (200, 20, 30)][c])
prev.resize((W * 2, H * 2), Image.NEAREST).save(out_png)

with open(out_h, 'w') as f:
    f.write('/* generato da logo2esl.py: BWR 400x300, piano B/W (1=bianco) + piano rosso */\n')
    f.write('static const uint8_t LOGO_FRAME[%d] = {\n' % len(frame))
    for i in range(0, len(frame), 20):
        f.write('  ' + ', '.join('0x%02x' % v for v in frame[i:i + 20]) + ',\n')
    f.write('};\n')
print('box', box, 'scaled', img.size)
