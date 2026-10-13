#! pychron: eqtime=20
# An air shot for the simulated lab: one pipette of the tank's air, let into
# the prep section and left there for the run's duration. The measurement
# plan then lets it into the spectrometer.
#
# The pipette `air` sits between P2 (to the tank) and P1 (to prep), which are
# interlocked: never both open.

def main():
    info('air shot {}'.format(run_identifier))
    close('C')        # prep off the turbo: whatever arrives now stays
    open('P2')        # the pipette fills from the tank
    sleep(2)          # ... in about a millisecond (0.1 cc through 0.1 L/s); 2 s is a valve's worth
    close('P2')
    open('P1')        # the shot expands into prep
    sleep(2)          # likewise
    close('P1')
    sleep(duration)   # the gas sits in prep, as a sample's would while it is cleaned up
