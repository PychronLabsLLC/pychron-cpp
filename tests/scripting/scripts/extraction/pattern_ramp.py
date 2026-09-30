import math


def main():
    enable()
    ramp(start=0, end=10, rate=1, period=2)
    execute_pattern('spiral')
    begin_interval(30)
    waitfor(lambda: is_open('A'), timeout=12)
    delay(math.floor(2.7))
    complete_interval()
