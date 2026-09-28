import sys, os, re
sys.stdout.reconfigure(encoding='utf-8')
from lxml import etree

base = r'D:\cubemx\esp32-s3\esp32_video\esp32_cam_video\unpacked-lby-20260916'
W = '{http://schemas.openxmlformats.org/wordprocessingml/2006/main}'

# media 图片
media = os.path.join(base, 'word', 'media')
if os.path.isdir(media):
    print('MEDIA files:')
    for f in sorted(os.listdir(media)):
        print('   ', f, os.path.getsize(os.path.join(media, f)), 'bytes')
else:
    print('no media dir')

tree = etree.parse(os.path.join(base, 'word', 'document.xml'))
body = tree.getroot().find(W + 'body')

def pinfo(p):
    texts = ''.join(t.text or '' for t in p.iter(W + 't'))
    pPr = p.find(W + 'pPr')
    sz = sorted(set(r.get(W + 'val') for r in p.iter(W + 'sz') if r.get(W + 'val')))
    spacing = pPr.find(W + 'spacing') if pPr is not None else None
    sp = {k.split('}')[1]: v for k, v in spacing.attrib.items()} if spacing is not None else {}
    ind = pPr.find(W + 'ind') if pPr is not None else None
    idd = {k.split('}')[1]: v for k, v in ind.attrib.items()} if ind is not None else {}
    fonts = sorted(set(
        (r.get(W + 'eastAsia') or r.get(W + 'ascii') or '')
        for r in p.iter(W + 'rFonts')))
    fonts = [f for f in fonts if f]
    bold = any(b.get(W + 'val') not in ('0', 'false') for b in p.iter(W + 'b'))
    return texts, sz, sp, idd, fonts, bold

print('=' * 90)
print('BODY direct children (block level):')
idx = 0
for child in body:
    tag = child.tag.split('}')[1]
    if tag == 'p':
        texts, sz, sp, idd, fonts, bold = pinfo(child)
        print(f'\n#{idx:02d} P  sz={sz} bold={bold} fonts={fonts}')
        print(f'      spacing={sp}')
        print(f'      ind={idd}')
        print(f'      text({len(texts)}): {texts[:95]}')
    elif tag == 'tbl':
        rows = child.findall(W + 'tr')
        tblPr = child.find(W + 'tblPr')
        floatpos = tblPr.find(W + 'tblpPr') if tblPr is not None else None
        fp = {k.split('}')[1]: v for k, v in floatpos.attrib.items()} if floatpos is not None else None
        # 单元格内段落
        npar = 0
        cellsum = []
        for tc in child.iter(W + 'tc'):
            ps = tc.findall(W + 'p')
            npar += len(ps)
            for pp in ps:
                texts, sz, sp, idd, fonts, bold = pinfo(pp)
                if texts.strip():
                    cellsum.append((len(texts), sz, sp, idd, bold, texts[:70]))
        print(f'\n#{idx:02d} TBL rows={len(rows)} paras_in_cells={npar} float={fp}')
        for cs in cellsum:
            print(f'      cellP len={cs[0]} sz={cs[1]} bold={cs[4]} spacing={cs[2]} ind={cs[3]}')
            print(f'            {cs[5]}')
    else:
        print(f'\n#{idx:02d} <{tag}>')
    idx += 1
