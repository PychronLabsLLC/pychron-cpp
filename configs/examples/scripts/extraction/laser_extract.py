#! pychron: eqtime=20
# Example laser extraction: isolate the prep section, drive the stage to the
# run's hole on the queue's tray, heat for the run's duration, then let the
# gas settle. The run switches the laser off afterwards whatever happens here.

def main():
    info('extracting {} at hole {}: {} {}'.format(run_identifier, position, extract_value, extract_units))
    close('C')
    move_to_position()
    enable()
    extract()
    fire_laser()
    sleep(duration)
    end_extract()
    disable()
    sleep(2)
