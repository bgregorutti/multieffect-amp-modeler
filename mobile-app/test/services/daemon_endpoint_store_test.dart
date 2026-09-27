import 'package:flutter_test/flutter_test.dart';
import 'package:mobile_app/services/daemon_endpoint_store.dart';
import 'package:shared_preferences/shared_preferences.dart';

void main() {
  setUp(() {
    // Starts every test with an empty backing store rather than whatever a
    // previous test (or the real platform channel) left behind.
    SharedPreferences.setMockInitialValues({});
  });

  test('load() returns null when nothing has been saved yet', () async {
    final store = DaemonEndpointStore();
    expect(await store.load(), isNull);
  });

  test('save() then load() round-trips the endpoint', () async {
    final store = DaemonEndpointStore();
    const endpoint = DaemonEndpoint(host: '192.168.1.42', port: 9000);

    await store.save(endpoint);

    expect(await store.load(), endpoint);
  });

  test('a later save() overwrites the earlier one', () async {
    final store = DaemonEndpointStore();
    await store.save(const DaemonEndpoint(host: '10.0.0.1', port: 8765));
    await store.save(const DaemonEndpoint(host: '10.0.0.2', port: 9001));

    expect(await store.load(), const DaemonEndpoint(host: '10.0.0.2', port: 9001));
  });

  test('DaemonEndpoint equality is by value', () {
    expect(
      const DaemonEndpoint(host: 'a', port: 1),
      const DaemonEndpoint(host: 'a', port: 1),
    );
    expect(
      const DaemonEndpoint(host: 'a', port: 1),
      isNot(const DaemonEndpoint(host: 'b', port: 1)),
    );
  });

  test('wsUri and httpBaseUrl are built from host and port', () {
    const endpoint = DaemonEndpoint(host: '192.168.1.42', port: 9000);
    expect(endpoint.wsUri, Uri.parse('ws://192.168.1.42:9000/ws'));
    expect(endpoint.httpBaseUrl, Uri.parse('http://192.168.1.42:9000'));
  });
}
