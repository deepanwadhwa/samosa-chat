# E-J2 Public Fetch Checkpoint

Generated: 2026-07-21T19:05:10.036960+00:00

Scope: public URL fetch and readable text extraction only. No model server was started; model-bearing checks remain Ornith-only.

## Summary

- URLs sampled: 20
- Clean extractions: 16/20
- JS failures flagged: 0
- Robots-disallowed fetches blocked: 0
- User-Agent: `SamosaChat/1.0 (+local user-initiated fetch)`
- Per-host minimum interval: 1.0s
- Acceptance target >=15/20 clean extractions: PASS

## Test Gate

- `python3 tests/test_gateway_web.py`: PASS (32 SSRF cases + tool protocol + search providers + robots/rate checks)

## Initial Rejected Candidates

- https://www.sqlite.org/careers.html: HTTP 404
- https://www.djangoproject.com/foundation/jobs/: HTTP 404
- https://www.postgresql.org/about/news/: HTTP 404
- https://openai.com/careers/search/: HTTP 403
- https://www.noaa.gov/work-with-us/careers: HTTP 403
- https://www.fsf.org/about/staff-and-board/job-opportunities: HTTP 404

## URL Results

| # | Result | URL | Final URL / Error | Title | Text chars |
|---:|---|---|---|---|---:|
| 1 | clean | https://www.python.org/jobs/ | https://www.python.org/jobs/ | www.python.org | 8773 |
| 2 | clean | https://www.mozilla.org/en-US/careers/listings/ | https://www.mozilla.org/en-US/careers/listings/ | Mozilla Careers — All open positions at Mozilla | 4996 |
| 3 | clean | https://www.rust-lang.org/governance | https://rust-lang.org/governance/ | Governance - Rust Programming Language | 2070 |
| 4 | clean | https://www.gnu.org/jobs/ | https://www.fsf.org/resources/jobs/ | Free software jobs — Free Software Foundation — Working together for free software | 5788 |
| 5 | clean | https://www.apple.com/careers/us/ | https://www.apple.com/careers/us/ | Apple Roles and Opportunities - Careers at Apple | 5553 |
| 6 | clean | https://www.microsoft.com/en-us/research/careers/ | https://www.microsoft.com/en-us/research/careers/ | Research Careers - Microsoft Research | 15168 |
| 7 | clean | https://www.nasa.gov/careers/ | https://www.nasa.gov/careers/ | Careers - NASA | 14412 |
| 8 | clean | https://www.w3.org/careers/ | https://www.w3.org/careers/ | Careers \| W3C | 2724 |
| 9 | clean | https://wikimediafoundation.org/about/jobs/ | https://wikimediafoundation.org/jobs/ | Work with us – Wikimedia Foundation | 21424 |
| 10 | clean | https://www.debian.org/intro/help | https://www.debian.org/intro/help | Debian -- Contribute: How you can help Debian | 8470 |
| 11 | clean | https://careers.state.gov/ | https://careers.state.gov/ | Home - Careers | 3119 |
| 12 | clean | https://www.usajobs.gov/ | https://www.usajobs.gov/ | USAJOBS - The Federal Government's official employment site | 7330 |
| 13 | clean | https://www.redhat.com/en/jobs | https://www.redhat.com/en/jobs | Red Hat Jobs \| Opportunities are open | 7447 |
| 14 | clean | https://jobs.cern/ | https://careers.cern/ | careers.cern | 15015 |
| 15 | fail | https://www.usa.gov/government-jobs | https://www.usa.gov/government-jobs | Redirecting to https://www.usa.gov/job-help | 89 |
| 16 | fail | https://www.nsa.gov/careers/ | fetch failed with HTTP 403 |  | 0 |
| 17 | clean | https://www.loc.gov/careers/ | https://www.loc.gov/careers/ | Careers at the Library of Congress \| The Library of Congress | 16001 |
| 18 | clean | https://www.worldbank.org/en/about/careers | https://www.worldbank.org/ext/en/careers | Careers \| World Bank Group | 6288 |
| 19 | fail | https://www.un.org/en/about-us/careers | fetch failed with HTTP 404 |  | 0 |
| 20 | fail | https://www.unicef.org/careers | fetch failed with HTTP 403 |  | 0 |
