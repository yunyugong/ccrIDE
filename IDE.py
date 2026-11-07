import tkinter as tk
from tkinter import filedialog, messagebox
import os
import subprocess
import time


# ==========================
# 基础
# ==========================

root = tk.Tk()
root.title("MiniCPP")
root.geometry("900x650")

file_path = None

# ==========================
# 数据盘选择
# D > E > F
# 禁止 C
# ==========================

SAVE = None

for d in ["D:\\", "E:\\", "F:\\"]:

    if os.path.exists(d):

        try:
            test = d + "MiniCPP_test"

            with open(test, "w") as f:
                f.write("")

            os.remove(test)

            SAVE = d
            break

        except:
            pass


if SAVE is None:

    messagebox.showerror(
        "MiniCPP",
        "没有可写入的数据盘"
    )

    exit()


DATA = SAVE + "MiniCPP"
BACKUP = DATA + "\\backup"

os.makedirs(
    BACKUP,
    exist_ok=True
)


# ==========================
# 查找 g++
# ==========================

gpp = None

search = [

    # 小熊猫C++
    r"D:\rred\mingw64\bin",
    r"E:\rred\mingw64\bin",

    # Dev-C++
    r"C:\Program Files (x86)\Dev-Cpp",

]


for folder in search:

    if os.path.exists(folder):

        for r, ds, fs in os.walk(folder):

            if "g++.exe" in fs:

                gpp = os.path.join(
                    r,
                    "g++.exe"
                )

                break

    if gpp:
        break


if gpp is None:

    messagebox.showerror(
        "MiniCPP",
        "找不到g++编译器"
    )

    exit()



# ==========================
# 编辑区
# ==========================

frame = tk.Frame(root)
frame.pack(
    fill="both",
    expand=True
)


lines = tk.Text(
    frame,
    width=5,
    state="disabled",
    font=("Consolas",12),
    bg="#eeeeee"
)

lines.pack(
    side="left",
    fill="y"
)


editor = tk.Text(
    frame,
    font=("Consolas",12),
    undo=True
)

editor.pack(
    side="left",
    fill="both",
    expand=True
)



# ==========================
# 行号
# ==========================

def update_line(e=None):

    lines.config(
        state="normal"
    )

    lines.delete(
        "1.0",
        "end"
    )

    n=int(
        editor.index(
            "end-1c"
        ).split(".")[0]
    )

    lines.insert(
        "1.0",
        "\n".join(
            str(i)
            for i in range(1,n+1)
        )
    )

    lines.config(
        state="disabled"
    )


editor.bind(
    "<KeyRelease>",
    update_line
)



# ==========================
# Tab
# ==========================

def tab(e):

    editor.insert(
        "insert",
        "    "
    )

    return "break"


editor.bind(
    "<Tab>",
    tab
)



# ==========================
# Enter 自动缩进
# ==========================

# ==========================
# 智能换行缩进
# ==========================

def enter(e):

    line = editor.index("insert").split(".")[0]

    current = editor.get(
        f"{line}.0",
        "insert"
    )

    # 空文件第一次回车
    if line == "1" and current == "":
        editor.insert(
            "insert",
            "\n"
        )
        return "break"


    indent = ""

    for c in current:
        if c == " ":
            indent += " "
        else:
            break


    code = current.strip()

    new_indent = indent


    if code.endswith("{"):
        new_indent += "    "


    if code.startswith("}"):
        if len(new_indent) >= 4:
            new_indent = new_indent[:-4]


    editor.insert(
        "insert",
        "\n" + new_indent
    )

    return "break"



# ==========================
# 自动括号
# ==========================

def bracket(e):

    pairs={

        "(" : ")",
        "[" : "]",
        "{" : "}",
        '"' : '"',
        "'" : "'"

    }


    if e.char in pairs:

        editor.insert(
            "insert",
            e.char+pairs[e.char]
        )

        editor.mark_set(
            "insert",
            "insert-1c"
        )

        return "break"


editor.bind(
    "<KeyPress>",
    bracket
)
# ==========================
# 智能退格
# ==========================

def backspace(e):

    pos = editor.index("insert")

    line = pos.split(".")[0]
    col = int(pos.split(".")[1])

    before = editor.get(
        line+".0",
        pos
    )

    # 光标前只有空格
    if before.strip()=="" and col>0:

        n=len(before)

        delete=min(
            4,
            n
        )

        editor.delete(
            f"{line}.{n-delete}",
            pos
        )

        return "break"


editor.bind(
    "<BackSpace>",
    backspace
)



# ==========================
# 保存
# ==========================

def save():

    global file_path


    if file_path is None:

        file_path=filedialog.asksaveasfilename(

            initialdir=SAVE,

            defaultextension=".cpp",

            filetypes=[
                ("C++","*.cpp")
            ]
        )


    if not file_path:
        return False


    try:

        with open(
            file_path,
            "w",
            encoding="utf-8"
        ) as f:

            f.write(
                editor.get(
                    "1.0",
                    "end-1c"
                )
            )

        return True


    except Exception as e:

        messagebox.showerror(
            "保存失败",
            str(e)
        )

        return False



def ctrl_s(e=None):

    save()

    return "break"



root.bind(
    "<Control-s>",
    ctrl_s
)



# ==========================
# 备份
# ==========================

def backup(e=None):

    name = (
        "backup_"
        +
        time.strftime(
            "%Y%m%d_%H%M%S"
        )
        +
        ".cpp"
    )


    try:

        with open(
            BACKUP+"\\"+name,
            "w",
            encoding="utf-8"
        ) as f:

            f.write(
                editor.get(
                    "1.0",
                    "end-1c"
                )
            )

    except:

        pass


    return "break"



root.bind(
    "<Control-Shift-S>",
    backup
)



# ==========================
# 自动保存
# ==========================

def autosave():

    try:

        with open(
            DATA+"\\autosave.cpp",
            "w",
            encoding="utf-8"
        ) as f:

            f.write(
                editor.get(
                    "1.0",
                    "end-1c"
                )
            )

    except:

        pass


    root.after(
        3000,
        autosave
    )



# ==========================
# 恢复
# ==========================

def recover():

    path = DATA+"\\autosave.cpp"


    if os.path.exists(path):

        try:

            with open(
                path,
                "r",
                encoding="utf-8"
            ) as f:

                code=f.read()


            if messagebox.askyesno(
                "恢复",
                "发现自动保存文件，恢复？"
            ):

                editor.insert(
                    "1.0",
                    code
                )


        except:

            pass



# ==========================
# F5 编译运行
# ==========================

def run(e=None):

    if not save():
        return "break"

    exe = (
        os.path.splitext(file_path)[0]
        + ".exe"
    )

    output.delete(
        "1.0",
        "end"
    )

    try:

        # 编译
        r = subprocess.run(
            [
                gpp,
                file_path,
                "-std=c++17",
                "-O2",
                "-o",
                exe
            ],
            cwd=os.path.dirname(file_path),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore"
        )


        if r.returncode != 0:

            output.insert(
                "end",
                r.stderr
            )

            return "break"


        output.insert(
            "end",
            "编译成功\n\n"
        )

        # 检查生成的exe

        output.insert(
            "end",
            "运行文件:\n" + exe + "\n\n"
        )

        if not os.path.exists(exe):

            output.insert(
                "end",
                "exe不存在\n"
            )

            return "break"

    
        # 运行程序

        exe_name = os.path.basename(exe)

        run_path = os.path.join(
            os.path.dirname(file_path),
            exe_name
        )

        r = subprocess.run(
            [
                run_path
            ],
            cwd=os.path.dirname(file_path),
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=10,
            creationflags=subprocess.CREATE_NO_WINDOW
        )


        output.insert(
            "end",
            r.stdout
        )


        output.insert(
            "end",
            r.stderr
        )


    except Exception as e:

        output.insert(
            "end",
            "错误:\n"
            + str(e)
        )


    return "break"



root.bind(
    "<F5>",
    run
)



# ==========================
# 输出
# ==========================

output=tk.Text(
    root,
    height=8,
    bg="black",
    fg="white"
)

output.pack(
    fill="x"
)



# ==========================
# 启动
# ==========================

root.after(
    100,
    recover
)

root.after(
    3000,
    autosave
)

root.after(
    100,
    update_line
)

root.mainloop()
