#! pychron: eqtime=20
# Example laser extraction: isolate the prep section, drive the stage to the
# run's hole on the queue's tray, heat for the run's duration (or for as long
# as the run's pattern takes, the beam moving along it), then let the gas
# settle. The run switches the laser off afterwards whatever happens here.
#
# The simulated grain gives no gas off, so for any run but a blank one
# pipette of the tank's air stands in for it (sim_air.py has the sequence
# and its waits).

def main():
    info('extracting {} at hole {}: {} {}'.format(run_identifier, position, extract_value, extract_units))
    close('C')
    if not analysis_type.startswith('blank'):
        open('P2')
        sleep(2)
        close('P2')
        open('P1')
        sleep(2)
        close('P1')
    move_to_position()
    enable()
    extract()
    fire_laser()
    if pattern:
        execute_pattern()   # a path, or a dragonfly following the glow for `duration`
    else:
        sleep(duration)
    end_extract()
    disable()
    sleep(2)
