#!/usr/bin/env python3
"""Generates a webnovel-omnibus-shaped EPUB (many small spine items) straight
to a zip file, for BookCatalog tests: the in-RAM Book needs hundreds of KB of
arena for this shape, the SD-backed catalog must stay under ~45 KB resident.

usage: gen_omnibus.py <out.epub> [chapter-count]
"""
import sys
import zipfile

OUT = sys.argv[1]
CHAPTERS = int(sys.argv[2]) if len(sys.argv) > 2 else 1700

container = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

manifest_items = [
    '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
    '<item id="css" href="styles.css" media-type="text/css"/>',
    '<item id="cover-img" href="images/cover.png" media-type="image/png" properties="cover-image"/>',
]
spine_items = []
nav_lis = []
chapters = []
for i in range(CHAPTERS):
    name = f"text/ch{i:04d}.xhtml"
    manifest_items.append(
        f'<item id="c{i:04d}" href="{name}" media-type="application/xhtml+xml"/>'
    )
    spine_items.append(f'<itemref idref="c{i:04d}"/>')
    frag = f"#sec{i}" if i % 3 == 0 else ""
    nav_lis.append(f'<li><a href="{name}{frag}">Chapter {i:04d}: The Long Road</a></li>')
    body = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ch</title></head>'
        f'<body><h1 id="sec{i}">Chapter {i:04d}</h1>'
        f"<p>Words of chapter {i} flow briefly here.</p></body></html>"
    )
    chapters.append((f"OEBPS/{name}", body))

opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:freeinkbook-omnibus-0001</dc:identifier>
    <dc:title>The Omnibus of a Thousand Chapters</dc:title>
    <dc:creator>Gen Erator</dc:creator>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    {chr(10).join(manifest_items)}
  </manifest>
  <spine>
    {chr(10).join(spine_items)}
  </spine>
</package>
"""

nav = f"""<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>TOC</title></head>
<body>
<nav epub:type="toc"><ol>
{chr(10).join(nav_lis)}
</ol></nav>
</body></html>
"""

css = "p { margin: 0.4em 0; }\n"
# Not a real PNG; the catalog only indexes the entry, nothing decodes it here.
cover = b"\x89PNG-fake-cover-bytes"

with zipfile.ZipFile(OUT, "w") as z:
    z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
    d = zipfile.ZIP_DEFLATED
    z.writestr("META-INF/container.xml", container, compress_type=d)
    z.writestr("OEBPS/content.opf", opf, compress_type=d)
    z.writestr("OEBPS/nav.xhtml", nav, compress_type=d)
    z.writestr("OEBPS/styles.css", css, compress_type=d)
    z.writestr("OEBPS/images/cover.png", cover, compress_type=d)
    for name, body in chapters:
        z.writestr(name, body, compress_type=d)

print(f"wrote {OUT}: {CHAPTERS} chapters")
