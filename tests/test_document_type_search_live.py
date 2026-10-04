#!/usr/bin/env python3
"""Real local query interpretation + OpenDecision + PDF reader, generated data only.

Requires an explicitly isolated SAMOSA_HOME and SAMOSA_TEST_URL. No user
inventory, conversations, logs or folders are consulted.
"""
import argparse, hashlib, json, os, re, subprocess, tempfile, time, urllib.request, urllib.error
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--fixture-root', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--reuse', action='store_true')
parser.add_argument('--case', action='append')
args = parser.parse_args()
home = Path(os.environ['SAMOSA_HOME']).resolve()
base = os.environ['SAMOSA_TEST_URL']
assert home != Path.home() / '.samosa' and ':8642' not in base, 'Use an isolated test application'
root = args.fixture_root.resolve()


def pdf(lines):
    parts = ['BT', '/F1 11 Tf', '50 740 Td']
    for line in lines:
        escaped = line.replace('\\', r'\\').replace('(', r'\(').replace(')', r'\)')
        parts.extend([f'({escaped}) Tj', '0 -18 Td'])
    stream = ('\n'.join(parts) + '\nET\n').encode()
    objects = [b'<< /Type /Catalog /Pages 2 0 R >>', b'<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>',
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
        f'<< /Length {len(stream)} >>\nstream\n'.encode() + stream + b'endstream']
    out = bytearray(b'%PDF-1.4\n%synthetic\n'); offsets = []
    for i, obj in enumerate(objects, 1):
        offsets.append(len(out)); out.extend(f'{i} 0 obj\n'.encode() + obj + b'\nendobj\n')
    xref = len(out); out.extend(b'xref\n0 6\n0000000000 65535 f \n')
    for offset in offsets: out.extend(f'{offset:010d} 00000 n \n'.encode())
    out.extend(f'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n'.encode())
    return bytes(out)


def raster_pdf(content):
    # Use the OS renderer, not a test Python imaging dependency. The resulting
    # PDF contains only a JPEG image, so the production OCR fallback is required.
    with tempfile.TemporaryDirectory(prefix="samosa-synthetic-scan-") as temporary:
        source = Path(temporary) / "form.pdf"; image = Path(temporary) / "form.jpg"
        source.write_bytes(content)
        subprocess.run(["sips", "-s", "format", "jpeg", str(source), "--out", str(image)], check=True, capture_output=True)
        dimensions = subprocess.check_output(["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(image)], text=True)
        width = int(re.search(r"pixelWidth: (\d+)", dimensions)[1]); height = int(re.search(r"pixelHeight: (\d+)", dimensions)[1])
        jpeg = image.read_bytes()
    stream = b"q 612 0 0 792 0 0 cm /Im0 Do Q\n"
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /XObject << /Im0 4 0 R >> >> /Contents 5 0 R >>",
        f"<< /Type /XObject /Subtype /Image /Width {width} /Height {height} /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length {len(jpeg)} >>\nstream\n".encode() + jpeg + b"\nendstream",
        f"<< /Length {len(stream)} >>\nstream\n".encode() + stream + b"endstream"]
    out = bytearray(b"%PDF-1.4\n%synthetic-image-only\n"); offsets = []
    for i, obj in enumerate(objects, 1):
        offsets.append(len(out)); out.extend(f"{i} 0 obj\n".encode() + obj + b"\nendobj\n")
    xref = len(out); out.extend(b"xref\n0 6\n0000000000 65535 f \n")
    for offset in offsets: out.extend(f"{offset:010d} 00000 n \n".encode())
    out.extend(f"trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode())
    return bytes(out)


forms = {
 'scan-042.pdf': ['Application to Extend/Change Nonimmigrant Status', 'Department of Homeland Security',
     'U.S. Citizenship and Immigration Services', 'Form I-539', 'Part 1. Information About You',
     'Family Name: Example One', 'Given Name: Alex', 'Mailing Address: 100 Example Street',
     'Part 2. Application Type', 'I am applying for an extension of stay.', 'Synthetic test form. Not for filing.'],
 'scan-087.pdf': ['Application to Extend/Change Nonimmigrant Status', 'Department of Homeland Security',
     'U.S. Citizenship and Immigration Services', 'Form I-539', 'Part 1. Information About You',
     'Family Name: Example Two', 'Given Name: Robin', 'Mailing Address: 200 Example Street',
     'Part 2. Application Type', 'I am applying for a change of status.', 'Synthetic test form. Not for filing.'],
 'research.pdf': ['Journal of Materials Science', 'Thermal conductivity of semiconductors and crystal lattice properties',
     'Abstract: This research investigates phonon scattering in nanoscale materials.', 'This is a research paper about heat conduction in semiconductors.'],
 'I539.pdf': ['Writing to Learn', 'Writing across the curriculum', 'Classroom teaching methods and educational outcomes.',
     'Teachers use reflective essays in education. This paper is about classroom writing.'],
 'investing.pdf': ['10 Investing Questions', 'Personal finance, investment strategy and asset allocation',
     'Compound interest, stock markets and portfolio management.'],
 'guide-019.pdf': ['Instructions for Application to Extend/Change Nonimmigrant Status', 'Form I-539 Instructions',
     'This guide explains how to fill in the application. It is not the application form.'],
 'notice-022.pdf': ['Notice of Action', 'USCIS Form I-797C', 'Case type: I-539 Application to Extend/Change Nonimmigrant Status',
     'This notice confirms receipt of an application and is not the application itself.'],
 'scan-118.pdf': ['Application for Employment Authorization', 'Department of Homeland Security',
     'U.S. Citizenship and Immigration Services', 'Form I-765', 'Part 1. Reason for Applying',
     'Part 2. Information About You', 'Family Name: Example Three', 'Given Name: Casey', 'Synthetic test form. Not for filing.'],
 'supplement-061.pdf': ['Supplement A to Application to Extend/Change Nonimmigrant Status',
     'U.S. Citizenship and Immigration Services', 'Form I-539A', 'Information about additional applicants',
     'Family Name: Example Four', 'Given Name: Jordan', 'This is a supplement, not the principal application form.'],
 'tax-209.pdf': ['Form W-8BEN', 'Certificate of Foreign Status of Beneficial Owner for United States Tax Withholding',
     'Department of the Treasury. Internal Revenue Service.', 'Part I. Identification of Beneficial Owner', 'Name: Example Taxpayer'],
 'opaque-731.pdf': ['Form AB-731', 'Application for Synthetic Equipment Access', 'Part 1. Applicant Information',
     'Family Name: Example Five', 'Given Name: Quinn', 'Part 2. Access requested',
     'A completely invented form identifier for generality testing, not a real government form.'],
 'scan-205.pdf': ['Form W-9', 'Request for Taxpayer Identification Number and Certification',
     'Department of the Treasury. Internal Revenue Service.', 'Name: Example Company',
     'Part I. Taxpayer Identification Number', 'Part II. Certification', 'Synthetic test form. Not for filing.'],
}
fixture = {name: pdf(lines) for name, lines in forms.items()}
fixture['raster-301.pdf'] = raster_pdf(fixture['scan-042.pdf'])
fixture['I539-unreadable.bin'] = b'\x00\x01\xff\x00' * 20
fixture['meeting.txt'] = b'The design team meeting is at 9 AM. Bring the prototype and notes.\n'
if args.reuse:
    assert all((root / name).read_bytes() == content for name, content in fixture.items()), 'Generated fixture changed'
else:
    assert not root.exists(), 'Use a new, separate generated folder'
    root.mkdir(parents=True)
    for name, content in fixture.items(): (root / name).write_bytes(content)


def request(path, body=None):
    token = (home / 'run/ui-token').read_text().strip()
    req = urllib.request.Request(base + path, data=json.dumps(body).encode() if body is not None else None,
        headers={'X-Samosa-Token': token, 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=900) as response: return json.load(response)


for _ in range(120):
    try:
        if request('/healthz').get('ready'): break
    except Exception: pass
    time.sleep(.5)
else: raise RuntimeError('The isolated model did not become ready')

results = {'fixture_root': str(root), 'fixture_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in fixture.items()},
    'base_url': base, 'health': request('/healthz'), 'cases': []}

def save():
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2))

cases = [
 ('form', 'find all documents that look like I539 in this folder.', {'scan-042.pdf', 'scan-087.pdf'}),
 ('instructions', 'Find I-539 instruction guides only.', {'guide-019.pdf'}),
 ('different_form', 'Find W-9 forms in this folder.', {'scan-205.pdf'}),
 ('receipt', 'Find receipt notices for I-539 applications.', {'notice-022.pdf'}),
 ('employment_form', 'Find completed or blank I-765 applications in this folder.', {'scan-118.pdf'}),
 ('novel_identifier', 'Find AB-731 application forms.', {'opaque-731.pdf'}),
 ('topic', 'Find research papers about semiconductor thermal conductivity.', {'research.pdf'}),
]
for name, goal, expected in cases:
    if args.case and name not in args.case: continue
    started = time.monotonic(); job_id = 'type-search-' + name + '-' + str(time.time_ns())
    row = {'case': name, 'goal': goal, 'job_id': job_id, 'expected': sorted(expected)}
    results['cases'].append(row); save()
    print(json.dumps({'stage':'started', 'case':name, 'job_id':job_id}), flush=True)
    try:
        route = request('/v1/jobs/decision/route', {'goal': goal}); row['route'] = route
        assert route['action'] == 'find', route
        data = request('/v1/jobs/decision/start', {'goal': goal, 'folder': str(root), 'job_id': job_id})
        row['response'] = data
        result = data['result']; items = result['items']; actual = {item['path'] for item in items if item['decision'] == 'match'}
        row['matches'] = sorted(actual)
        supported = actual - {'raster-301.pdf'} if name == 'form' else actual
        assert supported == expected, {'matches': sorted(actual), 'expected': sorted(expected), 'reviews': [i['path'] for i in items if i['needs_check']]}
        assert result['checked_files'] == len(fixture) and result['remaining_files'] == 0
        assert result['shortlist_count'] == len(actual)
        assert next(i for i in items if i['path'] == 'I539-unreadable.bin')['decision'] == 'needs_check'
        if name == 'form':
            raster = next(i for i in items if i['path'] == 'raster-301.pdf')
            assert raster['source'] in ('ocr', 'shared_pdf_cache')
            assert raster['decision'] in ('match', 'needs_check'), 'OCR ambiguity must remain a candidate, never a false exclusion'
            for path in ('guide-019.pdf', 'notice-022.pdf', 'I539.pdf', 'investing.pdf', 'research.pdf', 'scan-118.pdf', 'supplement-061.pdf', 'tax-209.pdf'):
                assert next(i for i in items if i['path'] == path)['decision'] != 'match', path
            selected = sorted(expected)
            request('/v1/jobs/selection', {'job_id': job_id, 'selected_paths': selected})
            reopened = request('/v1/jobs/decision/result?job_id=' + job_id)
            assert reopened['selected_paths'] == selected
            deeper = request('/v1/jobs/decision/next', {'job_id': job_id, 'action': 'deepen', 'selected_paths': selected})
            row['deepen_response'] = deeper
            assert deeper['result']['search_intent'] == result['search_intent'], 'Selected deepen changed the search intent'
            after = {i['path'] for i in deeper['result']['items'] if i['decision'] == 'match'}
            assert after == actual, {'before': sorted(actual), 'after': sorted(after), 'operation': 'selected deepen'}
        row['passed'] = True
    except Exception as error:
        row['passed'] = False; row['error'] = str(error)
        if isinstance(error, urllib.error.HTTPError): row['error_response'] = error.read().decode()
    row['elapsed_seconds'] = round(time.monotonic() - started, 3)
    save()
    print(json.dumps({k: v for k, v in row.items() if k not in ('response', 'deepen_response')}), flush=True)
assert results['cases'] and all(row['passed'] for row in results['cases']), 'Live search acceptance failed; see saved evidence'
