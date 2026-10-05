// Pure helpers for the ring selector in ConfigDialog, kept apart from the
// component so the request-building logic is unit-testable without
// rendering the dialog.

/**
 * Whether the dialog's current ring choice can be submitted.
 *
 * A ring selection is always required, on both create and edit: the service
 * rejects an explicit empty `ring_name`, so clearing the selector on edit
 * must block submission rather than silently resolve to "keep the bound
 * ring" (that meaning is reserved for leaving the selector untouched).
 */
export const isRingSelectionValid = (selectedRing: string): boolean => selectedRing.trim().length > 0;

/**
 * The `ring_name` value to send on `SetConfig`.
 *
 * Create always sends the selected ring. An edit that leaves the bound ring
 * unchanged omits the field so the service keeps the existing binding
 * instead of reading an explicit value; choosing a different ring sends it.
 * The caller only reaches this once `isRingSelectionValid` passes, so
 * `selectedRing` is never empty here.
 */
export const resolveRingNameForSubmit = (
    isCreate: boolean,
    selectedRing: string,
    boundRingName: string,
): string | undefined => {
    if (isCreate) return selectedRing;
    if (selectedRing === boundRingName) return undefined;
    return selectedRing;
};
