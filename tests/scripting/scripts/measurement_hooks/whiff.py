def before_main(api):
    api.position('Ar40', 'H1')
    api.add_conditional('Ar40 > 1000 -> truncate')
    api.log('hook ready')


def on_whiff_result(api, result):
    if result['Ar40'] > 10:
        api.truncate(quick=True)
    else:
        api.acquire(5, integration_time=1.048576)
