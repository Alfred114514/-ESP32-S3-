import re, sys
sys.stdout.reconfigure(encoding='utf-8')

p = r'D:\cubemx\esp32-s3\esp32_video\esp32_cam_video\unpacked-lby-20260916\word\document.xml'
c = open(p, encoding='utf-8').read()

# body 下的顶层块级元素（段落 + 表格），按出现顺序
body = re.search(r'<w:body>([\s\S]*)</w:body>', c).group(1)

blocks = re.findall(r'<w:(p|tbl)\b[\s\S]*?</w:\1>', body)
paras = re.findall(r'<w:p\b[\s\S]*?</w:p>|<w:p\b[^>]*/>', body)
tbls = re.findall(r'<w:tbl\b[\s\S]*?</w:tbl>', body)
print('top-level paragraphs:', len(paras), ' tables:', len(tbls))
print('=' * 80)

for i, pa in enumerate(paras):
    texts = ''.join(re.findall(r'<w:t[^>]*>([\s\S]*?)</w:t>', pa))
    szs = sorted(set(re.findall(r'<w:sz w:val="(\d+)"', pa)))
    fonts = sorted(set(re.findall(r'<w:rFonts[^/]*w:eastAsia="([^"]+)"', pa)))
    ascii_f = sorted(set(re.findall(r'<w:rFonts[^/]*w:ascii="([^"]+)"', pa)))
    style = re.search(r'<w:pStyle w:val="([^"]+)"', pa)
    spacing = re.search(r'<w:spacing[^/]*/>', pa)
    ind = re.search(r'<w:ind [^/]*/>', pa)
    jc = re.search(r'<w:jc w:val="([^"]+)"', pa)
    bold = '<w:b/>' in pa or '<w:b ' in pa
    snap = '<w:snapToGrid' in pa
    print(f'[{i:02d}] sz={szs} bold={bold} style={style.group(1) if style else "-"}')
    print(f'     ea={fonts} ascii={ascii_f} jc={jc.group(1) if jc else "-"}')
    print(f'     spacing={spacing.group(0) if spacing else "-"}')
    print(f'     ind={ind.group(0) if ind else "-"} snapToGrid={snap}')
    print(f'     len={len(texts)} | {texts[:80]}')

print('=' * 80)
print('TABLES:')
for ti, t in enumerate(tbls):
    rows = re.findall(r'<w:tr\b[\s\S]*?</w:tr>', t)
    print(f'-- table {ti}: rows={len(rows)}')
    grid = re.search(r'<w:tblGrid>([\s\S]*?)</w:tblGrid>', t)
    if grid:
        print('   gridCols:', re.findall(r'w:w="(\d+)"', grid.group(1)))
    tpr = re.search(r'<w:tblPr>([\s\S]*?)</w:tblPr>', t)
    if tpr:
        print('   tblPr:', re.sub(r'\s+', ' ', tpr.group(1))[:300])
    for ri, r in enumerate(rows):
        cells = re.findall(r'<w:tc\b[\s\S]*?</w:tc>', r)
        cellinfo = []
        for cc in cells:
            tcs = re.findall(r'<w:t[^>]*>([\s\S]*?)</w:t>', cc)
            csz = sorted(set(re.findall(r'<w:sz w:val="(\d+)"', cc)))
            csp = re.search(r'<w:spacing[^/]*/>', cc)
            cellinfo.append(f'[{"".join(tcs)[:30]}|sz={csz}|{"Y" if csp else "N"}]')
        print(f'   row{ri}: ' + ' '.join(cellinfo))

print('=' * 80)
print('STYLES.XML defaults:')
sp = r'D:\cubemx\esp32-s3\esp32_video\esp32_cam_video\unpacked-lby-20260916\word\styles.xml'
s = open(sp, encoding='utf-8').read()
dd = re.search(r'<w:docDefaults>[\s\S]*?</w:docDefaults>', s)
if dd:
    print(re.sub(r'\s+', ' ', dd.group(0))[:900])
for st in re.findall(r'<w:style [\s\S]*?</w:style>', s):
    sid = re.search(r'w:styleId="([^"]+)"', st)
    nm = re.search(r'<w:name w:val="([^"]+)"', st)
    if sid:
        spx = re.search(r'<w:spacing[^/]*/>', st)
        szx = re.search(r'<w:sz w:val="(\d+)"', st)
        print(f'  style {sid.group(1)} name={nm.group(1) if nm else "-"} sz={szx.group(1) if szx else "-"} spacing={spx.group(0) if spx else "-"}')
