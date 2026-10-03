# pychron data reduction

{% if data_source == "local" %}
Data: the database file `data/pychron.db` in this folder.
{% else %}
Data: the PostgreSQL database `{{ db_name }}` on {{ db_host }}:{{ db_port }} as `{{ db_user }}`.
The password is in `.pychron/credentials.toml` (this computer only).
{% endif %}

Open pychron and use File > Data Browser. `elctl doctor --install {{ install_name }}`
checks this setup.
