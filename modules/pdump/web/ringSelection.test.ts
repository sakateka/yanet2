import { describe, it, expect } from 'vitest';
import { isRingSelectionValid, resolveRingNameForSubmit } from './ringSelection';

describe('isRingSelectionValid', () => {
    it('requires a ring when creating a config', () => {
        expect(isRingSelectionValid('')).toBe(false);
        expect(isRingSelectionValid('  ')).toBe(false);
        expect(isRingSelectionValid('ring0')).toBe(true);
    });

    it('requires a ring when editing too, so clearing the selector cannot send an explicit empty ring_name', () => {
        expect(isRingSelectionValid('')).toBe(false);
    });
});

describe('resolveRingNameForSubmit', () => {
    it('always sends the selected ring on create', () => {
        expect(resolveRingNameForSubmit(true, 'ring0', '')).toBe('ring0');
    });

    it('omits ring_name on edit when the selection matches the bound ring, keeping the binding', () => {
        expect(resolveRingNameForSubmit(false, 'ring0', 'ring0')).toBeUndefined();
    });

    it('sends the new ring on edit when the selection differs from the bound ring', () => {
        expect(resolveRingNameForSubmit(false, 'ring1', 'ring0')).toBe('ring1');
    });
});
