#! pychron: eqtime=20
# A blank for the simulated lab: sim_air.py with the pipette left alone. The
# prep section is static for exactly as long as an air shot's, so what the
# measurement sees is what the walls gave off in that time and nothing else.

def main():
    info('blank {}'.format(run_identifier))
    close('C')        # prep off the turbo
    sleep(2)          # where the pipette would fill
    sleep(2)          # where it would empty into prep
    sleep(duration)   # static, as the shot would be
