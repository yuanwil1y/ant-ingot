import sys, subprocess, imageio_ffmpeg
from PIL import Image, ImageDraw
ff = imageio_ffmpeg.get_ffmpeg_exe()
video, out = sys.argv[1], sys.argv[2]
times = [float(x) for x in sys.argv[3:]]
ims = []
for t in times:
    raw = subprocess.run([ff, '-loglevel', 'error', '-ss', str(t), '-i', video, '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True).stdout
    im = Image.frombytes('RGB', (340, 640), raw[:340*640*3]).resize((170, 320))
    ims.append((t, im))
cols = min(8, len(ims)); rows = (len(ims) + cols - 1) // cols
S = Image.new('RGB', (cols * 180, rows * 340), (50, 50, 50))
d = ImageDraw.Draw(S)
for i, (t, im) in enumerate(ims):
    x, y = (i % cols) * 180 + 5, (i // cols) * 340 + 16
    S.paste(im, (x, y)); d.text((x, y - 14), f"{t:.2f}s", fill=(255, 255, 255))
S.save(out)
