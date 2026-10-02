#! pychron: eqtime=20
# Example extraction for the simulated lab: isolate the prep section from
# the turbo, "heat" for the run's duration, then let the gas settle.

def main():
    info('extracting {} at {} {}'.format(run_identifier, extract_value, extract_units))
    close('C')
    sleep(duration)
    sleep(2)
