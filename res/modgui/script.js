function (event) {
    function status2str(status) {
        switch (status) {
        case 1: return 'Stopped';
        case 2: return 'Starting';
        case 3: return 'Buffering';
        case 4: return 'Running';
        default: return 'Unavailable';
        }
    }
    function handle_event (symbol, value) {
        switch (symbol) {
            case 'status':
                event.icon.find ('[mod-port-symbol=status]').text (status2str (parseInt (value)));
                break;
            case 'bufferfill':
                event.icon.find ('[mod-port-symbol=bufferfill]').text ('' + value.toFixed(2) + ' %');
                break;
            case 'periods':
            case 'periodsize':
            case 'buffersize':
            case 'ratio':
                event.icon.find ('[mod-port-symbol=' + symbol + ']').text ('' + value);
                break;
        }
    }

    if (event.type == 'start') {
        var ports = event.ports;
        for (var p in ports) {
            handle_event (ports[p].symbol, ports[p].value);
        }
    }
    else if (event.type == 'change') {
        handle_event (event.symbol, event.value);
    }
}
