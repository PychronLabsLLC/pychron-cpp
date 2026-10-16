#! pychron: eqtime=20
# A cocktail shot for the simulated lab: one pipette of the cocktail tank's
# gas (sim.toml's built-in `cocktail`: Ar36 to Ar40 with Ar39 twenty times
# Ar36, as an irradiated standard gives), let into the prep section and left
# there for the run's duration. The measurement plan then lets it into the
# spectrometer, where its Ar39 shows on the detector the plan puts Ar39 on.
#
# Not in the shipped queue: an example of the second pipette at work. Name it
# as a run's extraction script, in place of sim_air, to run one.
#
# The pipette `cocktail` sits between Q2 (to the tank) and Q1 (to prep), which
# are interlocked: never both open.

def main():
    info('cocktail shot {}'.format(run_identifier))
    close('C')        # prep off the turbo: whatever arrives now stays
    open('Q2')        # the pipette fills from the tank
    sleep(2)          # ... in about a millisecond (0.1 cc through 0.1 L/s); 2 s is a valve's worth
    close('Q2')
    open('Q1')        # the shot expands into prep
    sleep(2)          # likewise
    close('Q1')
    sleep(duration)   # the gas sits in prep, as a sample's would while it is cleaned up
