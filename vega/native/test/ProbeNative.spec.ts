import {ProbeNative} from '../src';

jest.mock('../src/turbo-modules/ProbeNative');

describe('Template Turbomodule library tests', () => {
  it('Sample Test Case', () => {
    expect(ProbeNative.getMajorVersion()).toBe(1);
  });
});
