# Example post-measurement: pump the spectrometer and the prep section out
# through the turbo, then shut the spectrometer off again. Prep is left
# pumping, which is how the next run's extraction script expects to find it.
#
# The spectrometer empties through the inlet B into prep and on through C.
# With sim.toml's inlet that is a time constant of about 3 s, so 50 s takes
# an air shot (1e5 fA of Ar40) to a hundredth of a fA.

def main():
    signal_pump_time_start()
    open('C')
    open('B')
    info('pumping the spectrometer and prep after {}'.format(run_identifier))
    sleep(50)
    close('B')
