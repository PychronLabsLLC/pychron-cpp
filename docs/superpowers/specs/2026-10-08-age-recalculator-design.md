# Age recalculator

Date: 2026-10-08
Status: Design approved in outline; implementation deferred (no plan written yet)
Owner: Jake Ross
Depends on: `2026-10-02-arar-reduction-design.md` (the age equation 3.6,
`UFloat`, constants 5.3; this spec delivers its Phase 2 row `convert_age`).
Scope: recalculating published K-Ar and 40Ar/39Ar ages from the parameter set
they were published with (40K decay constants, K isotopic abundance, monitor
age) to another, with uncertainty. A Qt-free core in `libs/reduction`, a
library of named literature parameters, and `elctl recalc`.
Out of scope: analyses in the store (they are re-reduced, not converted, and
need J from monitors, which is Phase 2 of the reduction spec); a `pychron-ui`
window and a preview plot (a later spec on top of this core); Monte Carlo
propagation; the joint optimization of Renne et al. (2010).

## 1. Goal

Ages published at different times or by different laboratories assume
different decay constants, K abundances and monitor ages, and cannot be
compared until they are brought to one set. A user with a table of published
ages and the parameters each was published with gets the same ages under a
set of their choosing, each with an internal and an external uncertainty and
a record of every value used.

Success: `elctl recalc --in ages.csv --out recalculated.csv --to-decay
min2000 --to-monitor FCs:kuiper2008` converts a file that mixes K-Ar and
40Ar/39Ar ages published under different sets; a row whose source and
target sets are equal comes back unchanged; and changing the age of a
primary standard moves every age measured against a monitor intercalibrated
to it.

## 2. Prior art and what is not taken from it

The feature follows the idea of ArAR, the Argon Age Recalculator (Mercer and
Hodges, 2016, Chemical Geology 440, 148-163): named libraries of parameters,
both dating methods, a choice of which uncertainties are propagated, and a
log of the values used. The user guide cites it.

Nothing else comes from it. ArAR is a closed Java application: no code, no
library file, no text and no interface layout of it is used or consulted
during implementation. The mathematics here is the published method it
generalizes (section 3), derived in this document from the age equations.
Every library value (section 5) is transcribed from the primary paper that
reported it, and cites that paper.

## 3. Method

Recalculation recovers the quantity the laboratory measured, which no
parameter changes, and computes the age again from it.

Notation: `λ = λ_e + λ_β` the total 40K decay constant, `λ_e` the branch to
40Ar; `A` the atomic fraction 40K/K and `W` the atomic weight of K; `t_m` the
monitor age. A subscript 0 marks the set the age was published with.

### 3.1 40Ar/39Ar

`t = ln(1 + J F) / λ` and `J = (e^{λ t_m} - 1) / F_m`, so the measured
quantity is the ratio of the unknown's `F` to the monitor's:

    R = F / F_m = (e^{λ0 t0} - 1) / (e^{λ0 t_m0} - 1)
    t = ln(1 + R (e^{λ t_m} - 1)) / λ

The K abundance cancels and is not an input. (Renne et al., 1998; Dalrymple,
1979.)

### 3.2 K-Ar

`t = ln(1 + (λ / λ_e) (40Ar* / 40K)) / λ`. The laboratory measured 40Ar* and
K by weight; 40K follows from `A` and `W`. The measured quantity is

    X = 40Ar* / K = (λ_e0 / λ0) (e^{λ0 t0} - 1) A0 / W0
    t = ln(1 + (λ / λ_e) X W / A) / λ

(Dalrymple, 1979.) `W` is entered with the abundance set it belongs to.

### 3.3 Monitors and chains

A monitor's age is one of three kinds:

- Absolute: an age that does not depend on the 40K parameters, such as an
  astronomically calibrated one.
- K-Ar: a primary standard dated by K-Ar. It is stored as a reference age and
  the decay and abundance set that age was computed with; its age under any
  other set follows by 3.2.
- Intercalibrated: a ratio `R` to another monitor (the `R` of 3.1, as in
  Renne et al., 1998); its age under a set follows by 3.1 from the other
  monitor's age under that set.

Resolving a monitor under a parameter set walks the chain to an absolute or
K-Ar monitor. A change to a primary standard therefore reaches every monitor
intercalibrated to it and every age measured against those.

### 3.4 Uncertainty

Linear propagation with the existing `UFloat`. Two results per age:

- Internal: the published uncertainty carried through the transform, every
  parameter at its nominal value. Comparable between ages measured against
  the same monitor.
- External: internal plus the uncertainty of every target parameter the age
  depends on: decay constants, abundance, monitor age or the reference age
  and `R` values of its chain.

Each target parameter is one `UFloat` variable for the whole calculation, so
a decay constant that enters both the monitor's age and the unknown's is
counted once, with its sign.

The source set enters at nominal values only. What the published uncertainty
contains is stated with the age:

- `internal` (default): analytical, with or without the J measurement; no
  monitor age and no decay constant uncertainty.
- `full`: it also contains the source monitor age and decay constant
  uncertainties. Their first-order contributions, computed from the source
  set, are subtracted in quadrature to get the internal part. A negative
  remainder is an error for that row, not a zero.

### 3.5 Limits (stated in the user guide)

- A change in the atmospheric 40Ar/36Ar cannot be corrected from an age; it
  needs the isotope data.
- Monitor ages and decay constants are treated as independent. Sets that
  were determined jointly (Renne et al., 2010, 2011) have a covariance this
  ignores, which overstates the external uncertainty.
- Linear propagation; uncertainties large against the age are approximate.
- The recalculation is exact only for ages computed by the standard
  equations from one monitor; a plateau or isochron age is treated as one
  age measured against that monitor.

## 4. Core (`libs/reduction`)

New header `pychron/reduction/recalc.hpp`, sources globbed. Uses `Measured`,
`UFloat`, `Result` and `AgeUnits` as they are; `ReductionConstants` is not
changed.

    struct DecayConstants { Measured lambda_e, lambda_b; };        // 1/a
    struct KAbundance     { Measured k40_fraction, atomic_weight; };

    struct AbsoluteMonitor       { Measured age; };                // Ma
    struct KArMonitor            { Measured reference_age;
                                   DecayConstants decay; KAbundance abundance; };
    struct IntercalibratedMonitor{ Measured r; std::string relative_to; };
    struct MonitorDef            { std::string name;
                                   std::variant<AbsoluteMonitor, KArMonitor,
                                                IntercalibratedMonitor> kind; };

    struct ParameterSet { DecayConstants decay;
                          std::optional<KAbundance> abundance;     // K-Ar only
                          std::optional<std::string> monitor; };   // Ar/Ar only

    enum class PublishedError { Internal, Full };
    struct PublishedAge { Measured age; AgeUnits units; PublishedError error; };

    struct Recalculated { double age, internal_error, external_error;
                          double invariant;        // R or X
                          double percent_change; };

    Result<UFloat> resolve_monitor(const std::string& name,
                                   const DecayConstants&, const MonitorLookup&);
    Result<Recalculated> recalculate_arar(const PublishedAge&,
        const ParameterSet& from, const ParameterSet& to, const MonitorLookup&);
    Result<Recalculated> recalculate_kar(const PublishedAge&,
        const ParameterSet& from, const ParameterSet& to);

`MonitorLookup` is a function from a name to a `MonitorDef`, so the core does
not know the library. Errors, each a `Result` failure with the offending
name: unknown monitor, a cycle in a chain, a missing abundance for K-Ar or
monitor for Ar/Ar, a non-positive age or `1 + ...` argument, a negative
remainder in 3.4.

## 5. Parameter library

`pychron/reduction/recalc_library.hpp`: named entries of three kinds (decay
constants, K abundances, monitors), each with a `citation` string. A name is
`key` or, for a monitor, `mineral:key` (`FCs:kuiper2008`).

- Built-in entries are a table in code. Each is transcribed from its primary
  paper at implementation and has a row in the library test that pins the
  value and requires a non-empty citation. Intended first set: decay
  constants of Aldrich and Wetherill (1958), Steiger and Jäger (1977), Min
  et al. (2000), Renne et al. (2010, 2011); abundances of Nier (1950),
  Garner et al. (1975, the Steiger and Jäger value); monitor ages for FCs,
  ACs, TCs and GA-1550 from Renne et al. (1998), Kuiper et al. (2008),
  Rivera et al. (2011), Renne et al. (2011), with the Renne et al. (1998)
  intercalibration ratios. The existing preset values in `arar_types.cpp`
  stay where they are; the library's Steiger and Jäger entry is tested equal
  to them.
- User entries come from a TOML file (`tomlplusplus`, already a
  dependency), same three kinds, and override a built-in of the same name.
  A user entry without a citation is refused.

## 6. `elctl recalc`

`recalc.hpp` / `recalc.cpp` after the pattern of `export.cpp`. It needs no
store: `pychron_elctl_lib` links `pychron::reduction` unconditionally and the
command has no stub.

    elctl recalc arar --age 28.02 --error 0.28 --from-decay sj1977
        --from-monitor FCs:renne1998 --to-decay min2000 --to-monitor FCs:kuiper2008
    elctl recalc kar  --age 1.000 --error 0.005 --from-decay aw1958
        --from-abundance nier1950 --to-decay sj1977 --to-abundance sj1977
    elctl recalc --in ages.csv --out recalculated.csv --to-decay ... --to-monitor ...
        --to-abundance ...
    elctl recalc library

Common flags: `--units Ma|ka|a` (default Ma), `--sigma 1|2` (of the input and
the output, default 1), `--error-kind internal|full`, `--library <file>`.

The input CSV has a header and the columns `id, method, age, error,
from_decay, from_abundance, from_monitor`, and optionally `units` and
`error_kind`; the target set comes from the flags and is the same for every
row. The output repeats the input columns and adds the recalculated age, the
internal and external uncertainty, the percent change, and the value and
citation of every source and target parameter used: the output file is the
change log. A row that fails keeps its place, with the reason in an `error`
column and empty results; the command then exits `kFailed`. The CSV is RFC
4180, every row the width of the header, and a file written with `--out`
goes through `pychron::mark_as_user_file`.

`recalc library` lists every entry, built-in and user, with value and
citation.

## 7. Tests

`tests/reduction/test_recalc.cpp`, `test_recalc_library.cpp`;
`apps/elctl/tests/test_recalc_cmd.cpp`.

- Identity: equal source and target sets return the age and the internal
  uncertainty unchanged, for both methods.
- Round trip: A to B to A returns the original.
- Independent values: Dalrymple (1979) Table 2 for K-Ar (old to IUGS 1976
  constants); published recalculations of 40Ar/39Ar ages between FCs ages.
  These are numbers from the literature, not from another program.
- Derivatives: internal and external uncertainties against hand-derived
  partials at a young and an old age.
- Shared decay constant counted once: the external uncertainty of an age
  whose monitor is K-Ar dated differs from the sum in quadrature of the two
  contributions taken separately.
- Chains: changing a primary moves an intercalibrated monitor and the
  unknown; a cycle and an unknown name fail by name.
- `full` published error: the subtraction, and the negative remainder.
- Library: every built-in pinned with its citation; the Steiger and Jäger
  entry equals the preset; a user file overrides, and is refused without a
  citation.
- Command: usage errors, single age, CSV in and out with a failing row,
  width of every row, `library`.

## 8. Documentation

`docs/recalc.md`: what it does, the method and its limits (3.5), the
library and the user file, the command, and the citation of Mercer and
Hodges (2016) with the references of section 3. The Phase 2 row
`convert_age` of the reduction spec points here.

## 9. Open questions

- Whether a monitor defined by a measured 40Ar* and K content, rather than a
  reference age, is needed for the K-Ar kind.
- Whether the covariance of jointly determined sets (3.5) is worth a field
  on the library entry before the UI spec.
