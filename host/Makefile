CORE=../main/core
SRC=sim.c $(CORE)/scene.c $(CORE)/ingot.c $(CORE)/gfx.c $(CORE)/fonts.c $(CORE)/qrcodegen.c
FF=$(shell /root/esp/tools/venv/bin/python -c "import imageio_ffmpeg;print(imageio_ffmpeg.get_ffmpeg_exe())")
sim: $(SRC) $(CORE)/*.h
	gcc -O2 -o sim $(SRC) -lm
%.mp4: sim
	./sim $* | $(FF) -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 340x640 -r 30 -i - -c:v libx264 -pix_fmt yuv420p -crf 16 $@
