# -*- coding: utf-8 -*-
"""HsAudio 注册码生成器（Windows 版，源码形态）。

跟 macOS 上的注册机（tools/keygen/KeygenApp.cpp）和命令行版（tools/hskeygen.cpp）
是同一套算法、同一把产品密钥，生成的码可以互换：谁开都行，开到哪个名字就是哪个名字。

这是给作者自己用的工具，不要发给客户：它带着产品密钥。

双击 = 开窗；也可以当命令行用（CI 会用它跟 hskeygen 对拍）：

    HsKeygenWin.exe                      开窗
    HsKeygenWin-cli.exe code <名字>      生成一个注册码
    HsKeygenWin-cli.exe --batch <前缀> <n>   批量生成 n 个（名字 + 码）
    HsKeygenWin-cli.exe --check <注册码>  校验一个码
    HsKeygenWin-cli.exe --selftest       自检（必须输出 PASS）

打包成两个 exe：HsKeygenWin.exe（窗口版，PE 子系统 2 / --noconsole）和
HsKeygenWin-cli.exe（命令行版，PE 子系统 3 / console=True）。两种都可以带参数跑，
命令行版在真正的控制台里直接打印。
"""

import ctypes
import hashlib
import hmac
import os
import sys

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
MAC_BYTES = 6
MIN_BYTES = 1
MAX_BYTES = 24

# 产品密钥：不再存在源码里。用下面两种方式之一提供：
#   1) 环境变量 HS_PRODUCT_KEY（64 位十六进制，CI 从仓库 Secret 注入）；
#   2) 同目录下的 key.local.txt（本机自己放一行十六进制，已在 .gitignore）。
# 都没有就用 32 个 0 占位：窗口照常打开，但算出来的码跟插件对不上（--selftest 会 FAIL）。
def _load_product_key():
    text = (os.environ.get("HS_PRODUCT_KEY") or "").strip()

    if not text:
        local = os.path.join(os.path.dirname(os.path.abspath(__file__)), "key.local.txt")

        if os.path.exists(local):
            with open(local, "r") as handle:
                text = handle.read().strip()

    text = text.replace(" ", "").replace("\n", "").replace("\r", "")

    if len(text) == 64:
        try:
            return bytes.fromhex(text)
        except ValueError:
            pass

    return bytes(32)


PRODUCT_KEY = _load_product_key()
KEY_INJECTED = PRODUCT_KEY != bytes(32)


def base32_encode(data):
    """Crockford Base32，高位在前，末尾不足 5 位左对齐补零（与 C++ 完全一致）。"""
    out = []
    value = 0
    bits = 0

    for byte in data:
        value = (value << 8) | byte
        bits += 8

        while bits >= 5:
            bits -= 5
            out.append(ALPHABET[(value >> bits) & 31])

    if bits > 0:
        out.append(ALPHABET[(value << (5 - bits)) & 31])

    return "".join(out)


def base32_decode(text):
    """返回 bytes；遇到字母表外的字符返回 None。I/L 当 1、O 当 0（Crockford 的宽容）。"""
    out = bytearray()
    value = 0
    bits = 0

    for raw in text:
        char = raw.upper()

        if char in "- ":
            continue

        if char == "I" or char == "L":
            char = "1"
        elif char == "O":
            char = "0"

        index = ALPHABET.find(char)

        if index < 0:
            return None

        value = (value << 5) | index
        bits += 5

        if bits >= 8:
            bits -= 8
            out.append((value >> bits) & 0xFF)

    return bytes(out)


def make_code(serial):
    """名字 -> 注册码。名字按 UTF-8 算长度，1-24 字节；超了返回空串。"""
    name = serial.encode("utf-8")

    if len(name) < MIN_BYTES or len(name) > MAX_BYTES:
        return ""

    payload = bytes((0x01, len(name))) + name
    mac = hmac.new(PRODUCT_KEY, payload, hashlib.sha256).digest()[:MAC_BYTES]
    body = base32_encode(payload + mac)

    grouped = ["HS1-"]

    for i, char in enumerate(body):
        if i > 0 and i % 4 == 0:
            grouped.append("-")

        grouped.append(char)

    return "".join(grouped)


def verify_code(text):
    """注册码 -> 名字；无效返回 None。前缀和横线可有可无、大小写随便。"""
    body = "".join(text.split()).upper()

    if body.startswith("HS1"):
        body = body[3:]

    body = body.lstrip("-")
    blob = base32_decode(body)

    if blob is None or len(blob) < 2 + MAC_BYTES or len(blob) > 2 + MAX_BYTES + MAC_BYTES:
        return None

    if blob[0] != 0x01:
        return None

    size = blob[1]

    if size < MIN_BYTES or size > MAX_BYTES or len(blob) != 2 + size + MAC_BYTES:
        return None

    payload = blob[:2 + size]
    mac = hmac.new(PRODUCT_KEY, payload, hashlib.sha256).digest()[:MAC_BYTES]

    if mac != blob[2 + size:2 + size + MAC_BYTES]:
        return None

    try:
        return payload[2:].decode("utf-8")
    except UnicodeDecodeError:
        return None


# ---- 开窗 ------------------------------------------------------------------

SAMPLE = "HS1-041P-2RK3-RVNK-G818-70"   # make_code("abc")，跟 hskeygen --selftest 同一个向量

LOGO_PNG_B64 = (
    "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"
)   # ui/logo.png 缩到 46×46，内嵌（单文件 exe 也画得出来）

UI_W, UI_H = 620, 448
RADIUS_CARD = 10
RADIUS_CONTROL = 8

# 配色逐个取自 ui/Theme.h，一个色号都不改。
BACKGROUND = "#0e0f11"
SURFACE = "#17191c"
SURFACE_HIGH = "#1f2226"
STROKE = "#2b2f34"
TEXT_PRIMARY = "#f2f3f5"
TEXT_SECONDARY = "#8b9198"
TEXT_TERTIARY = "#5d6369"
ACCENT = "#b45ce0"
ACCENT_GLOW = "#c77fe8"        # accent.brighter(0.25)，码框文字
ACCENT_INK = "#0e0f11"         # 主按钮上的深色字
ACCENT_HOVER = "#c06ae6"
POSITIVE = "#4cd964"
WARNING = "#ffb020"
DANGER = "#ff5a52"
SELECTION = "#3d2e49"          # accentDim 叠在 surfaceHigh 上看起来的样子

ZH_FACES = ("Microsoft YaHei UI", "Microsoft YaHei", "SimHei")
MONO_FACES = ("Consolas", "Courier New")

TITLE_TEXT = "绘声Studio 注册码生成器"
BRAND = "绘声Studio"
SUBTITLE = "注册码生成器"
PLACEHOLDER = "比如：张三"
FOOTER_TEXT = "一个码对应一个名字，不绑机器、不联网、不会过期。"

MSG_INIT = "输入买家的名字，生成一串注册码发给他。"
MSG_EMPTY = "先写一个名字。"
MSG_TOO_LONG = "名字太长了：最多 24 个字节（差不多 8 个汉字）。"
MSG_SELFCHECK = "自检没通过，请把这串码和名字发回给开发者。"
MSG_OK = "装好之后，插件右下角会显示：已注册 · %s"
MSG_COPIED = "已复制到剪贴板。"


def pick_family(available, preferences, fallback):
    """挑第一个装了的字体，都没有就用 Tk 的兜底。"""
    for name in preferences:
        if name in available:
            return name

    return fallback


def _apply_dark_frame(win):
    """Windows 的标题栏和外框默认是浅色的；窗口是深色的，外面露一圈浅灰很难看。
    DWMWA_USE_IMMERSIVE_DARK_MODE=20（Win10 1809+ 是 19）+ DWMWA_BORDER_COLOR=34。
    老系统不认这几个属性就安静地跳过，绝不能让注册机打不开。"""
    try:
        import ctypes

        hwnd = ctypes.windll.user32.GetParent(win.winfo_id()) or win.winfo_id()
        dark = ctypes.c_int(1)
        for attr in (20, 19):
            if ctypes.windll.dwmapi.DwmSetWindowAttribute(
                    hwnd, attr, ctypes.byref(dark), ctypes.sizeof(dark)) == 0:
                break
        border = ctypes.c_uint32(0x00110F0E)      # COLORREF，0x00BBGGRR：#0e0f11
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 34, ctypes.byref(border), ctypes.sizeof(border))
        text = ctypes.c_uint32(0x00F5F3F2)        # 标题栏文字 #f2f3f5
        ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 36, ctypes.byref(text), ctypes.sizeof(text))
    except Exception:
        pass


def run_gui():
    """照 macOS 版（tools/keygen/KeygenApp.cpp）1:1 复刻的窗口。

    整块界面画在 Canvas 上：圆角卡片、圆角控件、页脚都是画出来的，只有名字框用一个
    真正的 Entry（内缩在画好的圆角底里，方角盖不到圆角）。坐标全部照搬 mac 版
    paint()/resized() 里算出来的数字。
    """
    import tkinter as tk
    import tkinter.font as tkfont

    # 高 DPI 屏上别让系统把窗口位图拉伸：按物理像素画，620×448 就是 620×448。
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    root = tk.Tk()
    root.title(TITLE_TEXT)
    root.configure(bg=BACKGROUND)
    root.resizable(False, False)

    families = set(tkfont.families(root))
    zh_face = pick_family(families, ZH_FACES, "TkDefaultFont")
    mono_face = pick_family(families, MONO_FACES, "Courier")

    def zh(size, weight="normal"):
        # Tk 里字号为负 = 像素高度；JUCE 的 size 也是像素，这样两边字号对得上。
        return (zh_face, -int(size), weight)

    canvas = tk.Canvas(root, width=UI_W, height=UI_H, bg=BACKGROUND,
                       highlightthickness=0, bd=0, takefocus=0)
    canvas.pack(fill="both", expand=True)

    def round_rect(x1, y1, x2, y2, radius, fill, outline=None, width=1, tags=()):
        r = min(radius, (x2 - x1) / 2.0, (y2 - y1) / 2.0)
        points = (x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r,
                  x2, y2 - r, x2, y2, x2 - r, y2, x1 + r, y2,
                  x1, y2, x1, y2 - r, x1, y1 + r, x1, y1)
        return canvas.create_polygon(points, smooth=True, splinesteps=24, fill=fill,
                                     outline=(outline or fill), width=width, tags=tags)

    # 卡片：mac 版 body = reduced(32,0).trimmedTop(104).trimmedBottom(96) → (32,104)-(588,352)
    round_rect(32, 104, 588, 352, RADIUS_CARD, SURFACE, STROKE, 1)

    # logo：内嵌的 ui/logo.png，画在 (32,28)，46×46
    logo = tk.PhotoImage(data=LOGO_PNG_B64)
    canvas.create_image(32, 28, image=logo, anchor="nw")
    root.iconphoto(True, logo)

    # 标题 22pt 加粗、副标题 13pt（mac: drawText 到 (92,26,320,28) / (92,52,320,20)，都是左对齐居中）
    canvas.create_text(92, 40, text=BRAND, anchor="w", fill=TEXT_PRIMARY, font=zh(22, "bold"))
    canvas.create_text(92, 62, text=SUBTITLE, anchor="w", fill=TEXT_SECONDARY, font=zh(13))

    # 名字输入框：(52,144) 516×34
    name_bg = round_rect(52, 144, 568, 178, RADIUS_CONTROL, SURFACE_HIGH, STROKE, 1)

    name_holder = tk.Frame(canvas, bg=SURFACE_HIGH, bd=0, highlightthickness=0)
    canvas.create_window(64, 148, anchor="nw", width=492, height=26, window=name_holder)

    name_var = tk.StringVar()
    name_box = tk.Entry(name_holder, textvariable=name_var, font=zh(16), bg=SURFACE_HIGH,
                        fg=TEXT_PRIMARY, relief="flat", bd=0, highlightthickness=0,
                        insertbackground=TEXT_PRIMARY, selectbackground=SELECTION,
                        selectforeground=TEXT_PRIMARY)
    name_box.place(x=0, y=0, relwidth=1, relheight=1)

    # 占位符跟 Entry 挤在同一个框里，谁盖在上面前后由 lift() 决定——不去动 Entry 里的字。
    hint = tk.Label(name_holder, text=PLACEHOLDER, bg=SURFACE_HIGH, fg=TEXT_TERTIARY,
                    font=zh(16), anchor="w", bd=0, highlightthickness=0)
    hint.place(x=0, y=0, relwidth=1, relheight=1)
    hint.bind("<Button-1>", lambda _e: name_box.focus_set())

    # 注册码行：(52,250) 412×40；右边「复制」按钮 92 宽、隔 12
    round_rect(52, 250, 464, 290, RADIUS_CONTROL, SURFACE_HIGH, STROKE, 1)
    code_item = canvas.create_text(258, 270, text="", anchor="center",
                                   fill=ACCENT_GLOW, font=(mono_face, -17))

    # 名字越长码越长（24 字节的名字有 68 个字符，17px 塞不进 412 宽的框里）。
    # 长码自动缩到塞得下为止，短码还是 17px，跟 mac 版一样。
    code_metrics = dict((s, tkfont.Font(family=mono_face, size=-s)) for s in range(9, 18))

    def code_font(text):
        for size in range(17, 8, -1):
            if code_metrics[size].measure(text) <= 402:
                return (mono_face, -size)

        return (mono_face, -9)

    # 状态行：卡片内 (52,304) 516×20 的左边
    status_item = canvas.create_text(52, 314, text=MSG_INIT, anchor="w",
                                     fill=TEXT_TERTIARY, font=zh(12))

    copy_job = [None]
    success = [""]

    def set_status(message, color):
        if copy_job[0] is not None:
            try:
                root.after_cancel(copy_job[0])
            except Exception:
                pass

            copy_job[0] = None

        canvas.itemconfigure(status_item, text=message, fill=color)

    def update_hint(*_args):
        # 名字框空了就把占位符盖上去，有字了让 Entry 露出来。
        (name_box if name_box.get() else hint).lift()

    def on_name_change(*_args):
        # mac 版 name_.onTextChange：名字一改，旧码就不作数了。
        canvas.itemconfigure(code_item, text="")
        set_status(MSG_INIT, TEXT_TERTIARY)
        update_hint()

    def generate(_event=None):
        name = name_box.get().strip()

        if not name:
            canvas.itemconfigure(code_item, text="")
            set_status(MSG_EMPTY, WARNING)
            return "break"

        code = make_code(name)

        if not code:
            canvas.itemconfigure(code_item, text="")
            set_status(MSG_TOO_LONG, DANGER)
            return "break"

        canvas.itemconfigure(code_item, text=code, font=code_font(code))

        # 出码前自己先验一遍，免得把错的码给买家。
        if verify_code(code) != name:
            set_status(MSG_SELFCHECK, DANGER)
            return "break"

        success[0] = MSG_OK % name
        set_status(success[0], POSITIVE)
        return "break"

    def copy_code():
        code = canvas.itemcget(code_item, "text")

        if not code:
            return

        root.clipboard_clear()
        root.clipboard_append(code)
        root.update()          # 让剪贴板里的码在窗口关掉之后也还在
        set_status(MSG_COPIED, TEXT_SECONDARY)
        copy_job[0] = root.after(2000, lambda: set_status(success[0], POSITIVE))

    # 按钮：圆角块 + 居中文字，两个 item 一起接事件，这样圆角外面点不到。
    buttons = {}

    def set_hover(active):
        for tag, (shape, fill, hover) in buttons.items():
            canvas.itemconfigure(shape, fill=(hover if tag == active else fill))

        canvas.configure(cursor=("hand2" if active else ""))

    def on_motion(_event=None):
        current = canvas.find_withtag("current")
        tags = set(canvas.gettags(current[0])) if current else set()
        set_hover(next((t for t in tags if t in buttons), None))

    def make_button(x, y, w, h, label, fill, ink, hover, command, size=15):
        tag = "hsbtn%d" % len(buttons)
        shape = round_rect(x, y, x + w, y + h, RADIUS_CONTROL, fill, None, 0, tags=(tag,))
        canvas.create_text(x + w / 2.0, y + h / 2.0, text=label, anchor="center",
                           fill=ink, font=zh(size), tags=(tag,))
        buttons[tag] = (shape, fill, hover)
        canvas.tag_bind(tag, "<Button-1>", lambda _e, run=command: run())

    make_button(52, 196, 150, 34, "生成注册码", ACCENT, ACCENT_INK, ACCENT_HOVER, generate)
    make_button(476, 250, 92, 40, "复制", SURFACE_HIGH, TEXT_PRIMARY, "#262a2f", copy_code, size=14)

    # 页脚：窗口底部 88 高、上留 12（mac: removeFromBottom(88).reduced(32,0).withTrimmedTop(12)）
    canvas.create_text(32, 372, text=FOOTER_TEXT, anchor="nw",
                       fill=TEXT_TERTIARY, font=zh(12))

    name_var.trace_add("write", on_name_change)
    name_box.bind("<Return>", generate)
    name_box.bind("<FocusIn>", lambda _e: canvas.itemconfigure(name_bg, outline=ACCENT))
    name_box.bind("<FocusOut>", lambda _e: canvas.itemconfigure(name_bg, outline=STROKE))
    root.bind("<Return>", generate)
    canvas.bind("<Motion>", on_motion)
    canvas.bind("<Leave>", lambda _e: set_hover(None))
    update_hint()

    root.update_idletasks()
    x = max(0, (root.winfo_screenwidth() - UI_W) // 2)
    y = max(0, (root.winfo_screenheight() - UI_H) // 2)
    root.geometry("%dx%d+%d+%d" % (UI_W, UI_H, x, y))
    root.update_idletasks()
    _apply_dark_frame(root)
    try:
        root.after(120, lambda: _apply_dark_frame(root))
    except Exception:
        pass
    name_box.focus_set()
    root.mainloop()


# ---- 命令行 ----------------------------------------------------------------

def attach_parent_console():
    """给「窗口版」（PE 子系统 2，没有自带控制台）补一个输出去处。

    在真控制台里敲 HsKeygenWin.exe --selftest 时：窗口版进程默认不接父进程的控制台，
    启动瞬间 Python 的 stdout/stderr 是 None，什么也打不出来。这里 AttachConsole 挂回
    父控制台再打开 CONOUT$，让窗口版也能在命令行里用。命令行版（子系统 3）本来就有
    控制台，GetConsoleWindow() 非 0，这里直接不插手。全程套 try，失败就算了。
    """
    try:
        if sys.stdout is not None and sys.stderr is not None:
            return

        kernel32 = ctypes.windll.kernel32

        if kernel32.GetConsoleWindow():
            return

        ATTACH_PARENT_PROCESS = -1

        if not kernel32.AttachConsole(ATTACH_PARENT_PROCESS):
            return

        if sys.stdout is None:
            sys.stdout = open("CONOUT$", "w", encoding="utf-8", buffering=1, closefd=False)

        if sys.stderr is None:
            sys.stderr = open("CONOUT$", "w", encoding="utf-8", buffering=1, closefd=False)
    except Exception:
        pass


def cli_program_name():
    """帮助里显示的程序名：真实 exe 名，不出现源文件名 .pyw。"""
    if getattr(sys, "frozen", False):
        return os.path.basename(sys.executable)

    script = os.path.basename(sys.argv[0] or "")

    if script.lower().endswith(".pyw"):
        script = script[:-4] + ".py"

    return script or "HsKeygenWin"


def usage():
    program = cli_program_name()
    sys.stdout.write(
        "HsAudio 注册码生成器（Windows）\n"
        "\n"
        "用法：%s <命令> [参数]\n"
        "  不带参数    开窗\n"
        "  code <名字>            生成一个注册码\n"
        "  --batch <前缀> <n>     批量生成 n 个（每行：名字 <Tab> 注册码）\n"
        "  --check <注册码>       校验一个码是谁的\n"
        "  --selftest             自检（必须打印 PASS）\n"
        "  --help                 显示这份帮助\n"
        "\n"
        "名字：1-24 字节的 UTF-8（汉字、英文、数字都行；一个汉字算 3 字节）。\n" % program)
    return 1


def selftest():
    code = make_code("abc")

    if code != SAMPLE:
        sys.stdout.write("FAIL: make_code(\"abc\") = %s\n" % code)
        return 1

    if verify_code(code) != "abc":
        sys.stdout.write("FAIL: verify_code() 拒绝了自己生成的码\n")
        return 1

    if verify_code(code[4:].replace("-", "")) != "abc":
        sys.stdout.write("FAIL: 去掉前缀和横线后校验失败\n")
        return 1

    if make_code("x" * 25) != "":
        sys.stdout.write("FAIL: 超长名字没有被拒绝\n")
        return 1

    if verify_code("HS1-0000-0000-0000-0000-00") is not None:
        sys.stdout.write("FAIL: 假码被当成有效\n")
        return 1

    sys.stdout.write("HsKeygenWin: PASS (%s)\n" % code)
    return 0


def main(argv):
    # 窗口版（--noconsole）打包成 exe 之后 sys.stdout 可能是 None：先试着挂回父控制台
    # （在命令行里敲窗口版时用），有重定向句柄就用重定向，两者都没有才丢进空设备。
    # 命令行分支照旧只写文本，不会因为 stdout 不存在而崩。
    attach_parent_console()

    if sys.stdout is None:
        try:
            sys.stdout = open(1, "w", encoding="utf-8", closefd=False)
        except Exception:
            sys.stdout = open(os.devnull, "w", encoding="utf-8")

    # 管道/重定向时 Python 默认用系统代码页写 stdout，中文名字会乱码；
    # 命令行模式下统一按 UTF-8 输出（控制台里 Python 自己会走 Unicode 接口，不受影响）。
    if hasattr(sys.stdout, "reconfigure") and not sys.stdout.isatty():
        try:
            sys.stdout.reconfigure(encoding="utf-8")
        except Exception:
            pass

    if len(argv) < 2:
        run_gui()
        return 0

    first = argv[1]

    if first in ("--help", "-h", "/?"):
        usage()
        return 0

    if first == "--selftest":
        return selftest()

    if first == "--check":
        if len(argv) < 3:
            return usage()

        name = verify_code(argv[2])

        if name is not None:
            sys.stdout.write("有效：%s\n" % name)
            return 0

        sys.stdout.write("无效：这个码不是本产品生成的，或者抄错了\n")
        return 1

    if first == "--batch":
        if len(argv) < 4:
            return usage()

        try:
            count = int(argv[3])
        except ValueError:
            return usage()

        # 每行「名字 <Tab> 注册码」，方便整块复制发给客户。
        for i in range(1, count + 1):
            name = "%s%02d" % (argv[2], i)
            sys.stdout.write("%s\t%s\n" % (name, make_code(name)))

        return 0

    if first == "code":
        if len(argv) < 3:
            return usage()

        name = argv[2]
        code = make_code(name)

        if not code:
            sys.stdout.write("名字必须是 1-24 字节（UTF-8）\n")
            return 1

        sys.stdout.write("%s\n" % code)
        return 0

    if first == "--gui":
        run_gui()
        return 0

    # 直接给名字也认（跟 hskeygen 一样）
    code = make_code(first)

    if not code:
        sys.stdout.write("名字必须是 1-24 字节（UTF-8）\n")
        return 1

    sys.stdout.write("%s\n" % code)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
