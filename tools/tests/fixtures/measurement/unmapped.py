#!Measurement
'''
equilibration:
  eqtime: 20
  inlet: R
  inlet_delay: 3
  outlet: O
multicollect:
  counts: 100
  detector: H1
  isotope: Ar40
'''


def main():
    activate_detectors('H1', 'AX')
    gosub('warm_cdd')
    equilibrate(eqtime=mx.equilibration.eqtime, inlet=mx.equilibration.inlet, outlet=mx.equilibration.outlet)
    position_magnet(mx.multicollect.isotope, detector=mx.multicollect.detector)
    multicollect(ncounts=mx.multicollect.counts)
    set_accelerating_voltage(4500)
