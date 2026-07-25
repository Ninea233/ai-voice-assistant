#!/usr/bin/env python3
"""
patch_tflite_headers_cxx14.py
将 TFLite 头文件中的 C++17 特性 patch 为 C++14 兼容。

修改内容：
1. inline constexpr → const
2. namespace A::B { → namespace A { namespace B {
3. 对应的闭合括号加倍 } → } }

运行一次即可，补丁是幂等的（第二次运行不做修改）。
如需恢复：git checkout third_party/tflite/include/
"""
import os
import re
import glob

TFLITE_INC = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..", "third_party", "tflite", "include"
)

HEADERS = glob.glob(os.path.join(TFLITE_INC, "tensorflow", "lite", "**", "*.h"),
                     recursive=True)
HEADERS += glob.glob(os.path.join(TFLITE_INC, "flatbuffers", "**", "*.h"),
                      recursive=True)
HEADERS += glob.glob(os.path.join(TFLITE_INC, "absl", "**", "*.h"),
                      recursive=True)
HEADERS += glob.glob(os.path.join(TFLITE_INC, "*.h"), recursive=False)

changes = 0

for hp in sorted(HEADERS):
    with open(hp, 'r', encoding='utf-8', errors='ignore') as f:
        content = f.read()
    original = content

    # 1. inline constexpr → const
    content = content.replace('inline constexpr', 'const')

    # 2. 3-level nested namespace: A::B::C {
    content = re.sub(
        r'^(\s*)namespace\s+([a-zA-Z_]\w*)::([a-zA-Z_]\w*)::([a-zA-Z_]\w*)\s*\{',
        r'\1namespace \2 { namespace \3 { namespace \4 {',
        content, flags=re.MULTILINE
    )

    # 3. 2-level nested namespace: A::B {
    content = re.sub(
        r'^(\s*)namespace\s+([a-zA-Z_]\w*)::([a-zA-Z_]\w*)\s*\{',
        r'\1namespace \2 { namespace \3 {',
        content, flags=re.MULTILINE
    )

    # 4. Fix closing braces: }  // namespace A::B → } }  // namespace A::B
    def fix_closing(m):
        indent = m.group(1)
        ns = m.group(2)
        count = ns.count('::') + 1
        braces = '} ' * count
        braces = braces.rstrip()
        return f'{indent}{braces}  // namespace {ns}\n'

    content = re.sub(
        r'^(\s*)\}\s*//\s*namespace\s+((?:[a-zA-Z_]\w*::)+[a-zA-Z_]\w*)\s*',
        fix_closing, content, flags=re.MULTILINE
    )

    if content != original:
        with open(hp, 'w', encoding='utf-8') as f:
            f.write(content)
        rel = os.path.relpath(hp, TFLITE_INC)
        print(f"  [patched] {rel}")
        changes += 1

print(f"\n完成！已 patch {changes} 个文件中的 C++17 特性。")
print("如需恢复: git checkout -- third_party/tflite/include/")
