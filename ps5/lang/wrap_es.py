#!/usr/bin/env python3
"""wrap_es - the Spanish dialogues, laid out to fit the game's text boxes.

The translation is written as flowing paragraphs in text_es/dialogs_es.txt;
the game, though, breaks its lines by hand. This measures each word with the
game's own character widths (gDialogCharWidths in src/game/ingame_menu.c) and
fills the lines up to the width the English text keeps to - 131 pixels, or 90
in the one narrow box - then writes text_es/dialogs.h in the game's format,
with every dialogue's own settings taken from the English one.

A dialogue with no translation yet keeps its English text.

    wrap_es.py <text/us/dialogs.h> <dialogs_es.txt> <charmap> <ingame_menu.c> <out dialogs.h>

In dialogs_es.txt:
    @DIALOG_014          starts a dialogue
    a blank line         a line break the translation asks for
    a line starting !    is written out as it is, unwrapped (the yes/no lines)
"""
import re
import sys

WIDE_LIMIT = 131
NARROW_LIMIT = 90


def read_widths(ingame_menu_c):
    src = open(ingame_menu_c).read()
    i = src.find('u8 gDialogCharWidths[256] = {')
    body = src[i:src.find('};', i)]
    body = re.sub(r'#ifdef VERSION_EU.*?#else(.*?)#endif', r'\1', body, flags=re.S)
    return [int(x) for x in re.findall(r'\b\d+\b', body.split('{', 1)[1])]


def read_charmap(path):
    cm = {}
    for line in open(path, encoding='utf-8'):
        m = re.match(r"^'(.+?)'\s*=\s*(.+?)\s*(#.*)?$", line.strip())
        if m:
            key = m.group(1).replace("\\'", "'")
            cm[key] = [int(x, 16) for x in m.group(2).replace(' ', '').split(',') if x]
    return cm


def read_english(path):
    """Every dialogue's settings and its English text, macros resolved."""
    src = open(path).read()
    src = re.sub(r'#ifdef VERSION_EU.*?#else(.*?)#endif', r'\1', src, flags=re.S)
    macros = dict(re.findall(r'#define (\w+) "(.*?)"', src))
    out = {}
    for m in re.finditer(r'DEFINE_DIALOG\((DIALOG_\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*_\((.*?)\)\)\s*\n',
                         src, re.S):
        did, unused, lines_per_box, left, width, raw = m.groups()
        # A macro between two string literals splices its text into them.
        raw = re.sub(r'"\s*(\w+)\s*"', lambda mm: macros[mm.group(1)], raw)
        text = ''.join(re.findall(r'"((?:\\.|[^"\\])*)"', raw, re.S))
        text = text.replace('\\\n', '').replace('\\n', '\n').replace("\\'", "'").replace('\\"', '"')
        out[did] = (int(unused), int(lines_per_box), int(left), int(width), text)
    return out


def read_spanish(path):
    """Each dialogue's translation: a list of paragraphs, a paragraph being a
    list of source lines (one of them may be marked verbatim)."""
    out, current = {}, None
    for line in open(path, encoding='utf-8').read().split('\n'):
        if line.startswith('#'):
            continue
        if line.startswith('@'):
            current = line[1:].strip()
            out[current] = []
        elif current is not None:
            out[current].append(line.rstrip())
    return out


def measure(text, cm, widths):
    keys = sorted(cm, key=len, reverse=True)
    total, i = 0, 0
    while i < len(text):
        for k in keys:
            if text.startswith(k, i):
                for c in cm[k]:
                    total += widths[c] if c < len(widths) else 0
                i += len(k)
                break
        else:
            raise SystemExit('wrap_es: no character %r in the charmap (%r)' % (text[i], text[:40]))
    return total


def options(line, cm, widths):
    """Where each word of a line like //Yes////No starts, in pixels. A slash
    is two spaces wide (DIALOG_CHAR_SLASH, src/game/ingame_menu.c)."""
    space = widths[0x9E]
    x, out = 0.0, []
    for part in re.findall(r'/+|[^/]+', line):
        if part.startswith('/'):
            x += len(part) * 2 * space
        else:
            word = part.lstrip(' ')
            x += (len(part) - len(word)) * space      # the gap belongs to the gap
            out.append((x, word))
            x += measure(word, cm, widths)
    return out


def align_choice(line, english_line, cm_es, cm_en, widths):
    """A dialogue's yes/no cursor is drawn at fixed places, so the words have
    to start where the English ones do. The gaps are rebuilt out of slashes and
    spaces to put them there."""
    space = widths[0x9E]
    mine = options(line, cm_es, widths)
    theirs = options(english_line, cm_en, widths)
    if len(mine) != len(theirs):
        return line
    out, x = '', 0.0
    for (_, word), (target, _) in zip(mine, theirs):
        gap = target - x
        while gap >= 2 * space:
            out += '/'
            gap -= 2 * space
            x += 2 * space
        while gap >= space:
            out += ' '
            gap -= space
            x += space
        out += word
        x += measure(word, cm_es, widths)
    return out


def wrap(paragraphs, limit, cm, widths):
    lines = []
    for para in paragraphs:
        if para.startswith('!'):
            lines.append(para[1:])
            continue
        if not para.strip():
            continue
        line = ''
        for word in para.split():
            candidate = word if not line else line + ' ' + word
            if line and measure(candidate, cm, widths) > limit:
                lines.append(line)
                line = word
            else:
                line = candidate
        if line:
            lines.append(line)
    return lines


def escape(line):
    return line.replace('\\', '\\\\').replace('"', '\\"')


def main(english_path, spanish_path, charmap_path, ingame_menu_c, out_path):
    widths = read_widths(ingame_menu_c)
    cm = read_charmap(charmap_path)
    cm_en = read_charmap('charmap.txt')
    english = read_english(english_path)
    spanish = read_spanish(spanish_path)

    unknown = [d for d in spanish if d not in english]
    if unknown:
        raise SystemExit('wrap_es: no such dialogue: %s' % ', '.join(unknown))

    translated = 0
    with open(out_path, 'w', encoding='utf-8') as out:
        out.write('// Generated by ps5/lang/wrap_es.py from dialogs_es.txt. Do not edit.\n'
                  '// Parameters: dialog enum ID, (unused), lines per box, left offset, width\n\n')
        for did, (unused, lines_per_box, left, width, en_text) in english.items():
            limit = NARROW_LIMIT if width <= 150 else WIDE_LIMIT
            if did in spanish and any(l.strip() for l in spanish[did]):
                lines = wrap(spanish[did], limit, cm, widths)
                translated += 1
                en_choices = [l for l in en_text.split('\n') if l.count('/') >= 2]
                if en_choices:
                    lines = [align_choice(l, en_choices[0], cm, cm_en, widths)
                             if l.count('/') >= 2 else l for l in lines]
            else:
                lines = en_text.split('\n')
            body = '\\n\\\n'.join(escape(l) for l in lines)
            out.write('DEFINE_DIALOG(%s, %d, %d, %d, %d, _("\\\n%s"))\n\n'
                      % (did, unused, lines_per_box, left, width, body))

    over = []
    for did in spanish:
        _, lpb, _, width, _ = english[did]
        limit = NARROW_LIMIT if width <= 150 else WIDE_LIMIT
        for line in wrap(spanish[did], limit, cm, widths):
            if measure(line, cm, widths) > limit:
                over.append((did, line))
    print('%s: %d of %d dialogues translated' % (out_path, translated, len(english)))
    for did, line in over:
        print('  too wide in %s: %r' % (did, line))


if __name__ == '__main__':
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
