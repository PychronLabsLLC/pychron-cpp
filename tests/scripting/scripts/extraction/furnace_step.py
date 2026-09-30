#! pychron: eqtime=30
def main():
    set_pid_parameters(extract_value)
    begin_heating_interval(duration)
    extract(extract_value, 'temp')
    complete_interval()
    dump_sample()
