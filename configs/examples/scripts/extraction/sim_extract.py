#! pychron: eqtime=20
# Example extraction for the simulated lab: isolate the prep section from
# the turbo, "heat" for the run's duration, then let the gas settle.
#
# Nothing in the simulated lab gives gas off when it is heated, so for any
# run but a blank one pipette of the tank's air stands in for the sample's
# gas (sim_air.py has the sequence and its waits). A blank gets none: it
# measures what the walls gave off while prep was shut.

def main():
    info('extracting {} at {} {}'.format(run_identifier, extract_value, extract_units))
    close('C')
    if not analysis_type.startswith('blank'):
        open('P2')
        sleep(2)
        close('P2')
        open('P1')
        sleep(2)
        close('P1')
    sleep(duration)
    sleep(2)
