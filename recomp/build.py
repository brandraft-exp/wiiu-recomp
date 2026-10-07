"""Emit a standalone Ninja build alongside generated C."""
from pathlib import Path
import shlex


def ninja_path(path):
    return str(path).replace('$', '$$').replace(' ', '$ ').replace(':', '$:')


def write_build(out, nfiles):
    runtime = Path(__file__).resolve().parent.parent / 'runtime'
    out = Path(out)
    (out / 'obj').mkdir(exist_ok=True)
    include = shlex.quote(str(runtime)).replace('$', '$$')
    lines = [
        'cc = clang',
        'cxx = clang++',
        'cflags = -O1 -std=c11 -fno-strict-aliasing -ffp-contract=off '
        '-Wno-unused-label -I' + include + ' -I.',
        'rule cc',
        '  command = $cc $cflags -MMD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule cxx',
        '  command = $cxx -O2 -std=c++20 -DENABLE_OPENGL -I' + include + ' -MMD -MF $out.d -c $in -o $out',
        '  depfile = $out.d',
        '  deps = gcc',
        'rule link',
        '  command = $cxx -rdynamic $in -o $out -lz -lm -pthread -lfmt -lEGL -lGL -lSDL2_ttf -lSDL2 -lavcodec -lswscale -lavutil',
    ]
    sources = [(f'funcs_{i:03d}', f'funcs_{i:03d}.c') for i in range(nfiles)]
    sources += [('table', 'table.c'), ('imports', 'imports.c'), ('data_imports', 'data_imports.c')]
    sources += [('rt_' + p.stem, p) for p in sorted((runtime / 'standalone').glob('*.c'))]
    sources += [('gx2_' + '_'.join(p.relative_to(runtime / 'gx2').with_suffix('').parts), p)
                for p in sorted((runtime / 'gx2').rglob('*.cpp'))]
    for name, path in sources:
        rule = 'cxx' if str(path).endswith('.cpp') else 'cc'
        lines.append(f'build obj/{name}.o: {rule} {ninja_path(path)}')
    lines.append('build game-native: link ' + ' '.join(f'obj/{n}.o' for n, _ in sources))
    lines.append('default game-native')
    (out / 'build.ninja').write_text('\n'.join(lines) + '\n')
