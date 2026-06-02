import http from 'k6/http';
import { SharedArray } from 'k6/data';
import exec from 'k6/execution';

const data = new SharedArray('d', () => JSON.parse(open('/test/test-data.json')).entries);
const TARGET = __ENV.TARGET || 'http://lb:9999';

const RATE = parseInt(__ENV.RATE || '0');   // >0 => constant arrival rate
const DUR  = __ENV.DUR || '20s';
export const options = {
    summaryTrendStats: ['p(99)', 'p(95)', 'avg', 'max'],
    scenarios: RATE > 0
      ? { d: { executor: 'constant-arrival-rate', rate: RATE, timeUnit: '1s',
               duration: DUR, preAllocatedVUs: 50, maxVUs: 400 } }
      : { d: { executor: 'ramping-arrival-rate', startRate: 1, timeUnit: '1s',
               preAllocatedVUs: 100, maxVUs: 400, stages: [{ duration: '30s', target: 900 }] } },
};

export default function () {
    const e = data[exec.scenario.iterationInTest % data.length];
    http.post(`${TARGET}/fraud-score`, JSON.stringify(e.request),
        { headers: { 'Content-Type': 'application/json' }, timeout: '2001ms' });
}
