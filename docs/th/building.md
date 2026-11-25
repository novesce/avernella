# การ build การรัน และการดีบัก

[สารบัญ](README.md) · ก่อนหน้า: [ภาพรวม](overview.md) · ถัดไป: [สถาปัตยกรรม](architecture.md)

## สิ่งที่ต้องมี

การ build ต้องใช้ **GNU GCC 12 ขึ้นไป** GNU Make, GNU binutils ที่มีลิงเกอร์ BFD (`ld.bfd`) และ `pkg-config` Makefile ตรวจว่า `$(CC)` เป็น GCC จริง (รัน `$(CC) -v` แล้วหาคำว่า `gcc version`) ถ้าไม่ใช่จะหยุดทันที ไม่รองรับการ build ด้วย Clang แต่ใช้ `clang-format` สำหรับจัดรูปแบบโค้ด

### ไลบรารี

นี่คือแพ็กเกจ `pkg-config` ในตัวแปร `PKGS` ด้านบนของ [Makefile](../../Makefile) ต้องมีครบทุกตัว

| แพ็กเกจ | ใช้ทำอะไร |
| ------- | --------- |
| `sdl2` (แนะนำ sdl2-compat บน SDL3) | หน้าต่าง อินพุต เสียง คลิปบอร์ด |
| `pangocairo` | การวาด (Cairo) และจัดวางข้อความ (Pango) รวมถึงภาษาไทยและอีโมจิ |
| `glib-2.0`, `gio-2.0`, `gio-unix-2.0` | โครงสร้างข้อมูล เธรด D-Bus (tray การแจ้งเตือน portal) |
| `json-c` | ข้อมูลที่มีโครงสร้างทั้งหมด รวมถึงตัวเชื่อม Thrift |
| `libcurl` | HTTPS พร้อม HTTP/2 |
| `libcrypto` (OpenSSL 3) | X25519, P-256, AES, HKDF, RSA และเลขสุ่ม |
| `libqrencode` | วาด QR code สำหรับลงชื่อเข้าใช้และเชิญเพื่อน |
| `libturbojpeg`, `libpng`, `libwebp` | ถอดรหัสรูปภาพ และตัวเข้ารหัส JPEG สำหรับรูปโปรไฟล์ |
| `zlib` | ข้อมูลประกาศการแชร์หน้าจอในสาย และ PNG |
| `opus` | เสียงในการโทร |
| `vpx` | วิดีโอ VP8 ในการโทร |
| `libsrtp2` | มีเดียการโทรที่เข้ารหัส |
| `libpipewire-0.3` | การแชร์หน้าจอ |
| `rnnoise` | การตัดเสียงรบกวนระหว่างโทร |

ส่วนที่เลือกได้ ระบบตรวจหาให้อัตโนมัติ

| ส่วน | แพ็กเกจ | ผลเมื่อไม่มี |
| ---- | ------- | ------------ |
| เบราว์เซอร์ WPE WebKit | `wpe-webkit-2.0 wpe-platform-wayland-2.0` | ไม่มี `avernella-liff` |
| เบราว์เซอร์ WebKitGTK | `webkitgtk-6.0` | ไม่มี `avernella-liff-gtk` ถ้าไม่มีเบราว์เซอร์เลย ลิงก์จะเปิดในเบราว์เซอร์ของระบบ |
| ตัววาด Vulkan แบบเนทีฟ | `glslangValidator`, `spirv-val`, header ของ Vulkan ที่ประกาศ `VK_EXT_descriptor_heap` และ volk (`/usr/include/volk.c` หรือ `-lvolk`) | ไม่มี `avernella-vulkan.so` แอปจะวาดผ่าน SDL แทน |
| ZBar | `libzbar.so.0` ตอนรัน โหลดด้วย `dlopen` ใน [`qr_scan.c`](../../src/qr_scan.c) | ใช้ **Read QR image** ไม่ได้ |
| เข้ารหัสและถอดรหัส VP8 สำหรับวิดีโอคอลด้วยฮาร์ดแวร์ | header ของ FFmpeg สำหรับ `libavcodec` และ `libavutil` (บน Ubuntu คือ `libavcodec-dev libavutil-dev` และบน Arch คือ `ffmpeg`) | แอปใช้ libvpx บน CPU ตั้ง `CALL_HWCODECS=0` เพื่อปิดการใช้ FFmpeg สำหรับวิดีโอคอล ตัวเลือกเดิม `CALL_HWDECODE=0` ยังใช้ได้ |
| ตัวเข้ารหัส VP8 บน Jetson | บน AArch64 ระบบบิลด์จะตรวจหา `gstreamer-app-1.0` และ `gstreamer-video-1.0` ส่วน Jetson Nano และ TX2 ต้องมีปลั๊กอิน `nvvidconv` และ `nvv4l2vp8enc` ของ JetPack | ตั้ง `CALL_VP8_JETSON=0` เพื่อบิลด์โดยปิดส่วนนี้ |
| FFmpeg | โปรแกรม `ffmpeg`, `ffplay`, `ffprobe` | อัดคลิปจากกล้องและเล่นวิดีโอไม่ได้ และเทสต์การอัดจะล้มเหลว |
| XDG desktop portal | `xdg-desktop-portal` พร้อม backend ของเดสก์ท็อปที่ใช้ | จับภาพหน้าจอและแชร์หน้าจอไม่ได้ |

บน Arch Linux หรือ CachyOS ดูคำสั่ง `pacman` ฉบับเต็มได้ใน README โดยสรุปคือ

```sh
sudo pacman -S --needed gcc binutils make pkgconf sdl2-compat \
  cairo pango glib2 qrencode zbar json-c curl openssl \
  libjpeg-turbo libpng libwebp opus libsrtp libvpx pipewire rnnoise
sudo pacman -S --needed vulkan-headers volk glslang spirv-tools vulkan-icd-loader   # โมดูล Vulkan
sudo pacman -S --needed wpewebkit webkitgtk-6.0                                     # เบราว์เซอร์ในแอป
sudo pacman -S --needed xdg-desktop-portal xdg-desktop-portal-xapp ffmpeg           # Cinnamon Wayland และวิดีโอ
sudo pacman -S --needed clang linux-headers xorg-server-xvfb                        # เครื่องมือพัฒนา
```

เลือก backend ของ portal ให้ตรงกับเดสก์ท็อปที่กำลังใช้งาน GNOME ใช้ `xdg-desktop-portal-gnome`, Plasma ใช้ `xdg-desktop-portal-kde` และ Hyprland ใช้ `xdg-desktop-portal-hyprland` ถ้าใช้ Cinnamon Wayland กับ XApp แอปสามารถให้ผู้ใช้เลือกจอภาพแล้วแชร์ผ่าน Muffin ได้

บนดิสทริบิวชันอื่น ให้ติดตั้งแพ็กเกจสำหรับพัฒนาที่มีชื่อ `pkg-config` ตรงกัน

## การ build

ที่รากของรีโพ

```sh
make            # avernella และ avernella-liff(-gtk) กับ avernella-vulkan.so เมื่อทำได้
make check      # build และรันทุกเทสต์ใน tests/
make bench      # เบนช์มาร์กการวาด
```

ไฟล์ object อยู่ใน `build/` โปรแกรมที่เสร็จแล้วจะถูกย้ายไปที่รากของรีโพ ระบบติดตามการพึ่งพา header ให้อัตโนมัติ (`-MMD -MP`) แก้ header เมื่อไร ไฟล์ที่ include มันจะถูก build ใหม่

GNU Make 4.3 ขึ้นไปจะรันงานพร้อมกันเท่าจำนวน CPU (`nproc`) โดยอัตโนมัติ เพราะ Makefile เติม `-j$(nproc)` ให้ถ้าคุณไม่ได้ระบุจำนวนงาน ใช้ `make -j1` ถ้าต้องการ build ทีละงาน เช่น เพื่ออ่านข้อความแจ้งข้อผิดพลาดตามลำดับ

### target ของ make

| Target | ทำอะไร |
| ------ | ------ |
| `all` (ค่าเริ่มต้น) | build `avernella` และโปรแกรมเสริมทุกตัวที่มี SDK ติดตั้งอยู่ |
| `check` | build ทุกเทสต์ใน `TESTS` แล้วรันทีละตัว หยุดเมื่อเจอตัวแรกที่ล้มเหลว |
| `bench` | build และรัน `build/render-bench` |
| `fuzz` | build ฟัซเซอร์ที่มี sanitizer และอินพุตตั้งต้นลงใน `build-fuzz/` ดู [การฟัซ](testing.md#การฟัซ) |
| `fuzz-run`, `fuzz-run-NAME` | รันทุกฟัซเซอร์หรือตัวเดียวเป็นเวลา `FUZZ_TIME` วินาที (ค่าเริ่มต้น 600) |
| `fuzz-check` | เล่นอินพุตตั้งต้นและ corpus ที่เก็บไว้ของแต่ละฟัซเซอร์ซ้ำหนึ่งรอบ |
| `vulkan-smoke` | เทสต์ตัววาด Vulkan แบบไม่แสดงผลบนจอ ใช้ `VK_VENDOR=<PCI vendor ID>` เช่น `0x10de` (NVIDIA) หรือ `0x1002` (AMD) เพื่อเลือก GPU |
| `vulkan-preview` | เทสต์ตัวนำเสนอ Vulkan ในหน้าต่างจริง ใส่ `VK_PREVIEW_ARGS=--test` สำหรับแบบอัตโนมัติ |
| `format` | จัดรูปแบบซอร์สทั้งหมดด้วย `clang-format` ทับไฟล์เดิม |
| `format-check` | ล้มเหลวถ้ามีซอร์สที่ยังไม่ได้จัดรูปแบบ |
| `checkpatch` | รัน `checkpatch.pl` ของเคอร์เนล Linux กับทุกซอร์ส ทีละไฟล์ |
| `install`, `uninstall` | ดูหัวข้อ "การติดตั้ง" ด้านล่าง |
| `clean` | ลบผลลัพธ์จากการ build แต่เก็บ corpus ของการฟัซไว้ `fuzz-clean` ลบ `build-fuzz/` ทั้งหมด |

### ตัวเลือกการ build

ส่งผ่านบรรทัดคำสั่ง เช่น `make DEBUG=1`

| ตัวเลือก | ผล |
| -------- | -- |
| `DEBUG=1` | build แบบ `-Og -g3` ไม่ strip มี frame pointer และ unwind table สำหรับใช้กับ `gdb` ต้องรัน `make clean` ทุกครั้งที่สลับระหว่าง debug กับ release เพราะไฟล์ object ไม่ถูก build ใหม่เมื่อเปลี่ยนแค่แฟล็ก |
| `LIFF_BROWSER=0` | ไม่ build เบราว์เซอร์ทั้งสองตัว |
| `NATIVE_VULKAN=1` / `0` | บังคับให้ต้องมีโมดูล Vulkan หรือไม่ build เลย ค่าเริ่มต้น `auto` จะ build เมื่อพบเครื่องมือ |
| `VOLK_DIR=…`, `VK_HEADERS=…` | build volk จากโค้ดที่ checkout มา หรือใช้ไดเรกทอรี include ของ Vulkan-Headers ที่กำหนด |
| `GLSLANG=…`, `SPIRV_VAL=…` | ตัวคอมไพล์และตัวตรวจ shader ที่จะใช้ |
| `CC=…` | ไดรเวอร์ GCC ตัวอื่น เช่น `gcc-14` หรือ wrapper คอมไพเลอร์ที่ไม่ใช่ GCC จะถูกปฏิเสธ |
| `CFLAGS=…` | แทนแฟล็กการปรับแต่ง (optimization) แฟล็กคำเตือนและแฟล็กความปลอดภัยยังถูกเติมให้เสมอ |
| `PREFIX=…`, `DESTDIR=…` | ตำแหน่งติดตั้ง ดูด้านล่าง |
| `CHECKPATCH=…`, `CLANG_FORMAT=…` | พาธของเครื่องมือตรวจสไตล์ |

### build แบบ release ทำอะไรบ้าง

แฟล็กเริ่มต้นคือ `-Oz -flto -fno-plt -fvisibility=hidden` โดยไม่มี unwind table บวก `-std=c17 -Wall -Wextra -Wpedantic -Wformat=2 -Werror=format-security -fstack-protector-strong -ffunction-sections -fdata-sections -fPIE` ถ้าคอมไพเลอร์รองรับ Makefile จะเพิ่ม `-fstack-clash-protection` และสำหรับ build ที่เปิด optimization และไม่ใช้ AddressSanitizer จะเพิ่ม `-D_FORTIFY_SOURCE=3` (แทนระดับที่ต่ำกว่าจาก environment) ตอนลิงก์ใช้ `-fuse-ld=bfd -pie`, `--gc-sections`, `--as-needed`, RELRO เต็มรูปแบบ, `-z separate-code`, `-z noexecstack` และถ้า toolchain รองรับก็ใช้ `--rosegment` กับ `-z pack-relative-relocs` (glibc 2.36 ขึ้นไป) ผลลัพธ์ถูก strip

มีบางไฟล์ที่ build เพื่อความเร็วแทนขนาด ได้แก่ `ui_pixels.c`, `camera_pixels.c` และ `screencast.c` ใช้ `-O3` เพื่อให้ GCC ทำ vectorize ลูปที่ประมวลผลทีละพิกเซล `screencast.c` ยังได้ `-Wno-pedantic` เพิ่ม เพราะ header ของ PipeWire ใช้ statement expression ของ GNU

### ไฟล์ที่สร้างขึ้นอัตโนมัติ

| ไฟล์ | สร้างจาก | ใช้โดย |
| ---- | -------- | ------ |
| `build/line_build_commit.h` | `git rev-parse HEAD` (7 ตัวแรก) หรือ `unknown commit` | ชื่อหน้าต่างและ **User Settings** (`ui_build_commit()`) เขียนใหม่เฉพาะเมื่อคอมมิตเปลี่ยน |
| `build/avernella_mark.inc`, `build/avernella_welcome.inc` | `data/avernella-mark.png`, `data/avernella-welcome.png` แปลงเป็นไบต์ด้วย `od` | [`ui_logo.c`](../../src/ui_logo.c): ไอคอนหน้าต่าง ไอคอน tray และภาพในหน้าลงชื่อเข้าใช้ |
| `build/ui_uber.vert.spv`, `build/ui_uber.frag.spv`, `build/line_ui_spirv.h` | `src/render/ui_uber.vert` และ `.frag` ผ่าน `glslangValidator` ตรวจด้วย `spirv-val` | shader ที่ฝังในโมดูล Vulkan |

`src/emoji_pack_index.inc` ซึ่งเป็นดัชนีออฟไลน์ของชุดอีโมจิสาธารณะ 45 ชุดของ LINE ถูกเก็บในรีโพ ส่วน `src/emoji_defaults.inc` ถูก git ละไว้ เป็นไฟล์ส่วนตัวที่ใช้เมื่อนิยาม `LINE_PRIVATE_EMOJI_LABELS` เท่านั้น การ build ปกติไม่ใช้

## การรัน

```sh
./avernella
```

ไม่มีตัวเลือกบรรทัดคำสั่งนอกจาก `--help` (`--worker` และ `--worker-password` ใช้ภายใน หน้าต่างใช้เพื่อเริ่ม worker เครือข่าย อย่ารันเอง)

ครั้งแรกจะเห็นหน้าลงชื่อเข้าใช้ เลือก **Sign in with QR code** แล้วสแกนด้วย LINE บนโทรศัพท์ หรือเลือก **Sign in with email** หลังลงชื่อเข้าใช้แล้ว เซสชันจะถูกบันทึก และครั้งต่อไปแอปจะใช้ต่อเองอัตโนมัติ ขั้นตอนลงชื่อเข้าใช้อธิบายไว้ใน [การลงชื่อเข้าใช้](sign-in.md)

เซสชันหนึ่งใช้ได้ทีละ Avernella เดียว เพราะไดเรกทอรีเซสชันถูกล็อกด้วย `flock` หน้าต่างที่สองจะแสดงข้อความ "Another Avernella window is using this session."

### Wayland และ X11

ในเซสชัน Wayland ไคลเอนต์ขอ SDL เป็น `wayland,x11` จึงใช้ Wayland โดยตรงและถอยไปใช้ X11 ถ้าไม่ได้ ตั้ง `SDL_VIDEODRIVER=x11` เพื่อบังคับ X11 บนแล็ปท็อปที่มีสอง GPU โมดูล Vulkan ของ Avernella จะลองใช้ GPU แยกของ NVIDIA ก่อน ตามด้วย GPU แยกยี่ห้ออื่นและกราฟิกในตัวตามลำดับ หากเปิดอุปกรณ์ไม่ได้จะลองตัวถัดไป และยังตรวจว่า GPU นั้นแสดงผลไปที่หน้าต่างได้ ถ้าต้องการใช้ GPU ในตัวเพื่อประหยัดแบตเตอรี่ ให้ตั้ง `AVERNELLA_RENDER_NODE` เป็น render node ของ GPU นั้น `LINE_RENDER_LOG=1` จะพิมพ์ชื่ออุปกรณ์ที่เลือก ส่วนเมื่อใช้ตัวเรนเดอร์ของ SDL `SDL_VIDEODRIVER=x11 prime-run ./avernella` จะเลือก GPU ของ NVIDIA ผ่าน X11 หรือ XWayland

## การติดตั้ง

build ด้วยผู้ใช้ปกติก่อน แล้วค่อยติดตั้งด้วย root

```sh
make
sudo make install            # ลงใน /usr/local
sudo make uninstall
make install PREFIX=$HOME/.local     # ติดตั้งเฉพาะผู้ใช้คนเดียว ไม่ต้องใช้ root
```

`make install` คัดลอกไฟล์ดังนี้

| จาก | ไปที่ |
| --- | ----- |
| `avernella` | `$(PREFIX)/bin/avernella` |
| `avernella-vulkan.so`, `avernella-liff`, `avernella-liff-gtk` (ตัวที่มี) | `$(PREFIX)/lib/avernella/` |
| `data/avernella.desktop` | `$(PREFIX)/share/applications/` |
| `data/avernella.png`, `data/avernella-mark.png` | ธีมไอคอน hicolor ขนาด 256 px และ 64 px |

ตอนรัน ไคลเอนต์หาโปรแกรมเสริมข้าง ๆ ไฟล์ตัวเอง หรือใน `../lib/avernella` เทียบกับตำแหน่งตัวเอง (อ่านจาก `/proc/self/exe`) จึงใช้ `PREFIX` ไหนก็ได้โดยไม่ต้อง build ใหม่ รองรับ `DESTDIR` สำหรับทำแพ็กเกจ app ID ของหน้าต่างคือ `avernella` ซึ่งเดสก์ท็อปใช้จับคู่กับไฟล์ `.desktop`

ผู้ใช้ Arch Linux และ CachyOS ติดตั้งแพ็กเกจสำเร็จรูป [avernella-bin](https://aur.archlinux.org/packages/avernella-bin) จาก AUR ได้

## ตัวแปรสภาพแวดล้อม

การใช้งานปกติไม่ต้องตั้งอะไรเลย ตัวแปรเหล่านี้มีไว้สำหรับดีบัก ปรับแต่ง และทดสอบ

### ข้อความดีบัก

| ตัวแปร | ผล | อ่านใน |
| ------ | -- | ------ |
| `LINE_DEBUG_TIMING=1` | พิมพ์ `line-timing <method> <ms>` สำหรับทุกคำขอไปยัง worker | `handle()` ใน `main.c` |
| `LINE_DEBUG_SYNC=1` | พิมพ์ผลของ long poll แต่ละรอบ: ผลลัพธ์ เวลา จำนวน operation | `live_sync.c` |
| `LINE_DEBUG_SESSION=1` | รายงานการต่ออายุโทเค็นและคำตอบโปรโตคอลที่ถอดรหัสไม่ได้ | `maintain_tokens()` ใน `main.c`, `line_decode_reply()` ใน `thrift.c` |
| `LINE_DEBUG_MEDIA=1` | รายงานขั้นตอนที่ล้มเหลว สถานะการรับส่ง และจำนวนไบต์ของรูปใน Flex หรือ rich message | `flex_asset.c`, `ui_media.c` |
| `LINE_DEBUG_LIFF=1` | รายงานขั้นตอนการโหลดหน้าเว็บและชื่อคำสั่ง bridge ที่รองรับ คำสั่งอื่นจะแสดงเป็น `unsupported` และยอมให้เบราว์เซอร์โหลดหน้า `http://127.0.0.1:<port>/` สำหรับทดสอบในเครื่อง | `liff_browser.c` |
| `LINE_CALL_DEBUG=1` | รายงานการต่อรองการโทร ข้อความควบคุม จำนวนแพ็กเก็ตทุกห้าวินาที และจังหวะที่ได้เฟรมแรก | `call.c`, `camera.c`, `ui_media.c` |
| `LINE_CALL_PDTP_DUMP=<dir>` | เมื่อเปิด `LINE_CALL_DEBUG=1` จะเก็บข้อมูลดิบของสตรีม LINE และ PLANET ในไดเรกทอรีที่สร้างไว้และให้สิทธิ์เฉพาะเจ้าของ (0700) | `call.c` |
| `LINE_RENDER_LOG=1` | บอกว่าเลือกตัววาดตัวไหนและทำไมต้องถอยไปตัวสำรอง | `ui_gpu.c`, `ui_draw.c` |
| `LINE_DEBUG_SANDBOX=1` | พิมพ์ว่าแซนด์บ็อกซ์ของ worker ชั้นใดทำงานอยู่: `line-sandbox no-dump=1 no-new-privs=1 seccomp=1 landlock-net=1 landlock-signal=1` | `sandbox.c` |

### พฤติกรรม

| ตัวแปร | ผล |
| ------ | -- |
| `LINE_EXPERIMENTAL_VIDEO=0` | ปิดวิดีโอคอล (โทรเสียงยังใช้ได้) |
| `LINE_DISABLE_SANDBOX=1` | เริ่ม worker โดยไม่มีแซนด์บ็อกซ์ เช่นเพื่อต่อ `gdb` หรือ `strace` เข้ากับ worker |
| `LINE_DISABLE_UDP_BATCH=1` | ส่งและรับมีเดียของการโทรครั้งละหนึ่งดาตาแกรมต่อ system call แทนการใช้ `sendmmsg`, `recvmmsg` และ `UDP_SEGMENT` |
| `LINE_NOISE_SUPPRESSION=0` | ส่งเสียงไมค์ดิบ ไม่ผ่าน RNNoise |
| `LINE_CALL_VIDEO_KBPS=<n>` | กำหนดบิตเรตการส่งวิดีโอเอง การโทรแบบตัวต่อตัวส่ง HD อย่างน้อย 1500 kbit/s เว้นแต่ตั้งค่านี้ให้ต่ำกว่า |
| `LINE_CAMERA_DEVICE=<path>` | อุปกรณ์กล้อง (ค่าเริ่มต้น `/dev/video0`) |
| `LINE_SHARE_WIDTH`, `LINE_SHARE_HEIGHT`, `LINE_SHARE_FPS`, `LINE_SHARE_KBPS` | เพดานของการจับและเข้ารหัสภาพตอนแชร์หน้าจอ |
| `LINE_SHARE_TRANSPORT=presentation` | ส่งการแชร์หน้าจอเป็น presentation ผ่าน PDTP แบบเนทีฟแทนการส่งในสตรีมวิดีโอ (ทดลอง) |
| `LINE_DISABLE_TRAY=1` | ปิดหน้าต่างแล้วออกจากโปรแกรมทันที แทนการซ่อนไว้ใน tray |
| `LINE_REDUCE_MOTION=1` | ปิดแอนิเมชัน (มีผลเหนือการตั้งค่าของเดสก์ท็อป) |
| `AVERNELLA_MAX_FPS=<n>` | จำกัดอัตราการวาดใหม่ เช่น `30` บนเครื่องรุ่นเก่า |

### การเลือกตัววาด

| ตัวแปร | ผล |
| ------ | -- |
| `LINE_DISABLE_GPU=1` | วาดลงพื้นผิวหน้าต่างของ SDL ใช้ libvpx สำหรับภาพวิดีโอคอลทั้งขารับและขาส่ง และเข้ารหัสข้อความวิดีโอด้วยซอฟต์แวร์ |
| `AVERNELLA_VULKAN=sdl` | ข้ามโมดูลเนทีฟ แต่ยังลองตัววาด Vulkan ของ SDL |
| `AVERNELLA_VK_DESCRIPTORS=buffer` | ให้โมดูลเนทีฟใช้ descriptor buffer แทน descriptor heap |
| `AVERNELLA_RENDER_NODE=/dev/dri/renderD128` | เลือก GPU ด้วย DRM render node บนไดรเวอร์ที่เปิดเผยคุณสมบัติ DRM |
| `SDL_VIDEODRIVER=x11` | บังคับใช้ X11 |

### เบนช์มาร์ก

| ตัวแปร | ผล |
| ------ | -- |
| `LINE_BENCH_VIDEODRIVER=x11` | รัน `render-bench` บน X11 แทนไดรเวอร์ dummy ของ SDL |
| `LINE_BENCH_DAMAGE=1` | เทียบเฟรมบางส่วนเฟรมสุดท้ายกับการวาดใหม่ทั้งจอด้วย |

## การดีบัก

- **คำขอล้มเหลว** ข้อความผิดพลาดจาก worker จะขึ้นที่บรรทัดสถานะด้านบนของแชต รันด้วย `LINE_DEBUG_TIMING=1` เพื่อดูว่าคำขอไหนล้มเหลวหรือช้า ข้อความอย่าง "LINE declined sendMessage (service code 10)" มีหมายเลขข้อผิดพลาดของ LINE เองติดมาด้วย หน้า [พื้นฐานโปรโตคอล](protocol-basics.md) อธิบายรหัสที่พบบ่อย
- **ข้อความมาช้า** รันด้วย `LINE_DEBUG_SYNC=1` ตอนว่าง long poll แต่ละรอบควรใช้เวลาราว 50 วินาที และตอบกลับทันทีเมื่อมีอะไรเกิดขึ้น
- **รูปในการ์ดว่างเปล่า** รันด้วย `LINE_DEBUG_MEDIA=1`
- **แอป LINE โหลดไม่เสร็จ** รันด้วย `LINE_DEBUG_LIFF=1` แล้วเปิดใหม่ ตรวจสอบว่าการโหลดถึง `committed` และ `finished` หรือยัง และมีคำสั่ง bridge ที่ขึ้นว่า `unsupported` หรือไม่
- **โทรแล้วไม่มีภาพ** รันด้วย `LINE_CALL_DEBUG=1`
- **ไล่โค้ดทีละขั้น** build ด้วย `make clean && make DEBUG=1` หน้าต่างคือโปรเซส `avernella` ส่วน worker เป็นโปรเซสลูกชื่อ `avernella --worker` ให้ attach แยกด้วย `gdb -p $(pgrep -f 'avernella --worker')` เนื่องจาก worker จะจบตามเมื่อโปรเซสแม่ตาย (`PR_SET_PDEATHSIG`) การปิดหน้าต่างจึงทำให้ worker จบด้วย
- **ทดสอบ `avernella-liff` ตัวเดียว** เริ่มมันโดยให้ file descriptor 3 เป็นซ็อกเก็ต `SOCK_SEQPACKET` และส่ง PID ของโปรเซสแม่เป็นอาร์กิวเมนต์เดียว แล้วส่ง datagram `L\tfull\t<url>` ไปหา เมื่อตั้ง `LINE_DEBUG_LIFF=1` มันจะโหลดหน้าจาก `http://127.0.0.1:<port>/` ได้ด้วย หน้าทดสอบในเครื่องที่โหลด LIFF SDK ตัวจริงจึงใช้ทดสอบ bridge ได้
- **เซสชันดูเหมือนเสีย** ออกจากระบบจากในแอป ถ้าทำไม่ได้ ให้ปิดแอปแล้วลบ `~/.local/state/avernella/session.json` จากนั้นลบอุปกรณ์ที่เชื่อมไว้ใน LINE บนโทรศัพท์

## ข้อมูลของคุณอยู่ที่ไหน

| พาธ | เนื้อหา |
| --- | ------- |
| `$XDG_STATE_HOME/avernella/` (ค่าเริ่มต้น `~/.local/state/avernella/`) | `session.json` (โทเค็น MID กุญแจ Letter Sealing ลำดับคำขอ) และ `lists-<sha256 ของ MID>.json` (โฟลเดอร์ โฟลเดอร์ที่เลือกอยู่ หมุด แชตที่ปิดเสียง ปุ่มลัด สวิตช์การแจ้งเตือนและเสียง โหมดลดการเคลื่อนไหว) |
| `$XDG_CONFIG_HOME/avernella/` (ค่าเริ่มต้น `~/.config/avernella/`) | `layout` (ธีมที่เลือก) และภาษาของส่วนติดต่อที่เลือก |
| `$XDG_RUNTIME_DIR/avernella/` | รูปที่วางเพื่อเตรียมส่ง ลบทิ้งหลังใช้ |
| `~/.cache/avernella/liff`, `~/.local/share/avernella/liff` | แคช HTTP และคุกกี้ของเบราว์เซอร์ในแอป |

รายละเอียดของแต่ละไฟล์อยู่ใน [สถาปัตยกรรม](architecture.md)
