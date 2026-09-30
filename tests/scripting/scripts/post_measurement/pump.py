def main():
    if get_intensity('Ar40') > 100:
        info('large signal; extra pumping')
    open('A')
    signal_pump_time_start()
