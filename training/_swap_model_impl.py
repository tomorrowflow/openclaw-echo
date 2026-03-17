#!/usr/bin/env python3
"""
Internal implementation for swap_model.sh.
Updates CMakeLists.txt and micro_wake.cpp to use a new wake word model.
"""
import json
import os
import re
import sys


def asm_symbol(filename: str) -> str:
    """Convert filename to linker symbol (non-alnum → underscore)."""
    return re.sub(r'[^a-zA-Z0-9]', '_', filename)


def load_manifest(json_path: str) -> dict:
    """Load training manifest JSON, return config dict."""
    with open(json_path) as f:
        d = json.load(f)
    sliding = d.get('sliding_window_size', d.get('sliding_window_average_size', 10))
    arena = d.get('tensor_arena_size', 46000)
    cutoff_float = d.get('probability_cutoff', 0.97)
    cutoff_u8 = int(round(cutoff_float * 255))
    return {
        'sliding_window': sliding,
        'tensor_arena_size': arena,
        'cutoff_u8': cutoff_u8,
        'cutoff_float': cutoff_float,
    }


def update_cmake(cmake_path: str, new_model: str, assets_dir: str):
    """Replace the wake model filename in EMBED_FILES (not vad.tflite)."""
    with open(cmake_path) as f:
        content = f.read()

    # Find any .tflite in EMBED_FILES that isn't vad.tflite
    pattern = r'(assets/)([a-zA-Z0-9_]+)(\.tflite)(.*?vad\.tflite)'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        print(f"  Warning: Could not find wake model in EMBED_FILES, trying simpler pattern")
        # Fallback: replace any assets/*.tflite that isn't vad
        old = content
        # Find all tflite references
        for m in re.finditer(r'assets/([a-zA-Z0-9_]+)\.tflite', content):
            name = m.group(1)
            if name != 'vad':
                content = content.replace(f'assets/{name}.tflite', f'assets/{new_model}')
                break
        if content == old:
            print("  Error: Could not find wake model reference in CMakeLists.txt")
            sys.exit(1)
    else:
        old_ref = f'assets/{match.group(2)}.tflite'
        content = content.replace(old_ref, f'assets/{new_model}')

    with open(cmake_path, 'w') as f:
        f.write(content)

    # Remove old model file if it's different from the new one
    old_name = match.group(2) + '.tflite' if match else None
    if old_name and old_name != new_model:
        old_path = os.path.join(assets_dir, old_name)
        if os.path.exists(old_path):
            os.remove(old_path)
            print(f"  Removed old model: {old_name}")

    print(f"  CMakeLists.txt: EMBED_FILES updated to assets/{new_model}")


def update_micro_wake(wake_path: str, new_model: str, config: dict):
    """Update asm symbols and config defines in micro_wake.cpp."""
    with open(wake_path) as f:
        lines = f.readlines()

    new_sym = asm_symbol(new_model)  # e.g., "hey_snorri_tflite"
    changes = []

    for i, line in enumerate(lines):
        # Update asm symbols for wake model (lines with _start/_end but NOT vad)
        # Match: asm("_binary_ANYTHING_start") or asm("_binary_ANYTHING_end")
        # but only on lines that don't contain "vad"
        if 'asm(' in line and 'vad' not in line.lower():
            m = re.search(r'asm\("_binary_(\w+?)_(start|end)"\)', line)
            if m:
                old_sym = m.group(1)
                suffix = m.group(2)
                # Update asm symbol
                lines[i] = lines[i].replace(
                    f'_binary_{old_sym}_{suffix}',
                    f'_binary_{new_sym}_{suffix}'
                )
                changes.append(f'  asm symbol: _binary_{old_sym}_{suffix} -> _binary_{new_sym}_{suffix}')
                # Also update the C variable name on this extern line
                m_var = re.search(r'\b(\w+_tflite)_(start|end)\b', line)
                if m_var:
                    old_var = f'{m_var.group(1)}_{m_var.group(2)}'
                    new_var = f'{new_sym}_{m_var.group(2)}'
                    if old_var != new_var:
                        lines[i] = lines[i].replace(old_var, new_var)
                        changes.append(f'  extern var: {old_var} -> {new_var}')

        # Update variable names (e.g., hey_jarvis_tflite_start used later in code)
        # but NOT on asm() lines (already handled) and NOT vad lines
        elif 'vad' not in line.lower():
            # Match old wake model variable references
            m_var = re.search(r'\b(\w+_tflite)_(start|end)\b', line)
            if m_var and m_var.group(1) + '_' + m_var.group(2) not in ('vad_tflite_start', 'vad_tflite_end'):
                old_var = f'{m_var.group(1)}_{m_var.group(2)}'
                new_var = f'{new_sym}_{m_var.group(2)}'
                if old_var != new_var:
                    lines[i] = line.replace(old_var, new_var)
                    changes.append(f'  variable: {old_var} -> {new_var}')

        # Update config defines
        if line.startswith('#define WAKE_SLIDING_WINDOW'):
            old = line.rstrip()
            lines[i] = f'#define WAKE_SLIDING_WINDOW      {config["sliding_window"]}\n'
            if old != lines[i].rstrip():
                changes.append(f'  WAKE_SLIDING_WINDOW: {config["sliding_window"]}')

        if line.startswith('#define WAKE_TENSOR_ARENA_SIZE'):
            old = line.rstrip()
            lines[i] = f'#define WAKE_TENSOR_ARENA_SIZE   {config["tensor_arena_size"]}\n'
            if old != lines[i].rstrip():
                changes.append(f'  WAKE_TENSOR_ARENA_SIZE: {config["tensor_arena_size"]}')

        if line.startswith('#define WAKE_CUTOFF_U8'):
            old = line.rstrip()
            lines[i] = f'#define WAKE_CUTOFF_U8           {config["cutoff_u8"]}\n'
            if old != lines[i].rstrip():
                changes.append(f'  WAKE_CUTOFF_U8: {config["cutoff_u8"]}')

        # Update comment about cutoff
        if 'probability_cutoff' in line and 'WAKE' in lines[i-1] if i > 0 else False:
            lines[i] = f'/* probability_cutoff {config.get("cutoff_float", 0.97)} -> in uint8 scale: {config.get("cutoff_float", 0.97)} * 255 ≈ {config["cutoff_u8"]} */\n'

        # Update header comment about model filename
        if 'feeds them to' in line and '.tflite' in line:
            lines[i] = re.sub(r'feeds them to \S+\.tflite', f'feeds them to {new_model}', line)

    with open(wake_path, 'w') as f:
        f.writelines(lines)

    if changes:
        print("  micro_wake.cpp changes:")
        for c in changes:
            print(c)
    else:
        print("  micro_wake.cpp: no changes needed (already configured)")


def main():
    if len(sys.argv) < 5:
        print("Usage: _swap_model_impl.py <model.tflite> <cmake_path> <wake_path> <assets_dir> [manifest.json]")
        sys.exit(1)

    model_file = sys.argv[1]
    cmake_path = sys.argv[2]
    wake_path = sys.argv[3]
    assets_dir = sys.argv[4]
    json_path = sys.argv[5] if len(sys.argv) > 5 else None

    # Load config from manifest or use defaults
    if json_path and os.path.isfile(json_path):
        config = load_manifest(json_path)
        print(f"Config from {os.path.basename(json_path)}:")
        print(f"  sliding_window={config['sliding_window']}, "
              f"arena={config['tensor_arena_size']}, "
              f"cutoff={config['cutoff_float']} (u8={config['cutoff_u8']})")
    else:
        config = {
            'sliding_window': 10,
            'tensor_arena_size': 46000,
            'cutoff_u8': 247,
            'cutoff_float': 0.97,
        }
        if json_path:
            print(f"Warning: Manifest not found: {json_path}")
        print("Using default config (edit micro_wake.cpp manually if needed)")

    print()
    print("2. Updating CMakeLists.txt")
    update_cmake(cmake_path, model_file, assets_dir)

    print("3. Updating micro_wake.cpp")
    update_micro_wake(wake_path, model_file, config)


if __name__ == '__main__':
    main()
