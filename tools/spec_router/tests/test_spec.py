from spec_router.spec import Section, parse_sections

SAMPLE = """# Title

Intro paragraph.

## 1. Intent

Intent body.

## 4. Core types

### 4.1 Errors

```cpp
enum class ErrorKind { Timeout };
```

### 4.2 Transport

Transport body.

## 9. Milestone

### 9.4 Order of work

1. `core`: Result
2. `transport`: Sim
"""


def test_parse_splits_on_h2_and_h3():
    secs = parse_sections(SAMPLE)
    ids = [s.id for s in secs]
    assert ids == ["1", "4", "4.1", "4.2", "9", "9.4"]


def test_section_fields():
    secs = {s.id: s for s in parse_sections(SAMPLE)}
    s = secs["4.1"]
    assert isinstance(s, Section)
    assert s.level == 3
    assert s.heading == "Errors"
    assert s.path == ["Core types", "Errors"]
    assert "enum class ErrorKind" in s.body
    # body excludes the heading line and any child headings
    assert "###" not in s.body


def test_parent_body_excludes_children():
    secs = {s.id: s for s in parse_sections(SAMPLE)}
    assert secs["4"].body.strip() == ""
    assert "Transport body" not in secs["4"].body


def test_preamble_before_first_h2_is_dropped():
    secs = parse_sections(SAMPLE)
    assert all("Intro paragraph" not in s.body for s in secs)


def test_heading_without_number_gets_synthetic_id():
    text = "## Alpha\n\nx\n\n### Beta\n\ny\n"
    secs = parse_sections(text)
    assert [s.id for s in secs] == ["alpha", "alpha.beta"]
