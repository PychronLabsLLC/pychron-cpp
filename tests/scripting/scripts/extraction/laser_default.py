#! pychron: eqtime=20, label="CO2 default"
# Ported from pychron's felix_laser extraction script: docstring metadata
# moved to the header line above; ex/mx globals are gone.


def main():
    info('extracting {}'.format(run_identifier))
    gosub('common:prepare_line')
    enable()
    move_to_position(position)
    extract(extract_value)
    fire_laser()
    sleep(duration)
    end_extract()
    disable()
    sleep(cleanup)
