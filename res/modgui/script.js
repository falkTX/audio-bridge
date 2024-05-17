function (event) {
    function handle_event (symbol, value) {
        switch (symbol) {
            case 'distance':
                event.icon.find ('[mod-port-symbol=distance]').text ('' + value + ' samples');
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
