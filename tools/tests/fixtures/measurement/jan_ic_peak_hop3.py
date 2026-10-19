#!Measurement
'''
'''
# Detector intercalibration: a per-run field table from a peak center on
# H1, AX and L2, then Ar40 hopped onto every cup.
ICHOPS = 'ic_hops'


def main():
    info('Jan IC peak hop')
    set_deflection('CDD', 2000)
    set_integration_time(1)
    generate_ic_mftable(('H1', 'AX', 'L2'), refiso='Ar40', name='ic_run')
    equilibrate(eqtime=15, inlet='R', outlet='O', delay=3)
    set_fits()
    set_baseline_fits()
    load_hops(ICHOPS)
    peak_hop(ncycles=6, hops=ICHOPS, mftable='ic_run')
    set_time_zero()
