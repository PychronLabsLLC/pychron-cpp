from spec_router.units import load_units, waves


def test_default_units_load_and_are_unique():
    units = load_units()
    ids = [u.id for u in units]
    assert len(ids) == len(set(ids))
    assert "core" in ids and "hardware_bringup" in ids


def test_dependencies_resolve_and_point_to_earlier_waves():
    units = {u.id: u for u in load_units()}
    for u in units.values():
        for d in u.depends:
            assert d in units, f"{u.id} depends on unknown {d}"
            assert units[d].wave < u.wave, f"{u.id} depends on same/later wave {d}"


def test_waves_group_in_order_and_skip_manual():
    ws = waves(load_units(), include_manual=False)
    assert [w.number for w in ws] == sorted(w.number for w in ws)
    assert all(not u.manual for w in ws for u in w.units)
    first = ws[0]
    assert [u.id for u in first.units] == ["core"]
