import React, { useState } from 'react';
import { Icon, TextInput } from '@gravity-ui/uikit';
import { Plus } from '@gravity-ui/icons';
import {
    RING_DEFAULT_PUBLISH_BATCH,
    RING_MAX_CAPACITY,
    RING_MAX_PUBLISH_BATCH,
    RING_MIN_CAPACITY,
    type RingInfo,
} from '@yanet/core/api/ring';
import { formatBytes } from '@yanet/core/utils';
import { TrashIcon } from '@yanet/core/components/draft';

const isPowerOfTwo = (value: bigint): boolean => value > 0n && (value & (value - 1n)) === 0n;

/** Parses a capacity textbox value: must be a power-of-two byte count within the service's accepted range. */
export const parseCapacityInput = (value: string): bigint | null => {
    if (!/^\d+$/.test(value.trim())) return null;
    try {
        const parsed = BigInt(value.trim());
        if (parsed < BigInt(RING_MIN_CAPACITY) || parsed > BigInt(RING_MAX_CAPACITY)) return null;
        return isPowerOfTwo(parsed) ? parsed : null;
    } catch {
        return null;
    }
};

/** Whether a publish-batch textbox value can be submitted: empty (the service default), or an integer up to the service's maximum. */
export const isValidPublishBatchInput = (value: string): boolean => {
    const trimmed = value.trim();
    if (trimmed === '') return true;
    return /^\d+$/.test(trimmed) && Number(trimmed) <= RING_MAX_PUBLISH_BATCH;
};

/** The publish_batch value to send for a validated textbox value; empty omits the field. */
export const resolvePublishBatchForSubmit = (value: string): number | undefined => {
    const trimmed = value.trim();
    return trimmed === '' ? undefined : Number(trimmed);
};

const ringCapacityBigInt = (ring: RingInfo): bigint => {
    try {
        return BigInt(ring.capacity ?? 0);
    } catch {
        return 0n;
    }
};

interface RingsPanelProps {
    rings: RingInfo[];
    loading: boolean;
    busy: boolean;
    onCreate: (name: string, capacity: number, publishBatch?: number) => Promise<boolean>;
    onDeleteRequest: (name: string) => void;
}

/**
 * Ring objects list with inline create and per-row delete.
 *
 * A pdump config links one of these rings by name; deleting a bound
 * ring is refused by the service and surfaces as a toast.
 */
const RingsPanel: React.FC<RingsPanelProps> = ({ rings, loading, busy, onCreate, onDeleteRequest }) => {
    const [draftName, setDraftName] = useState('');
    const [draftCapacity, setDraftCapacity] = useState('');
    const [draftPublishBatch, setDraftPublishBatch] = useState('');

    const trimmedName = draftName.trim();
    const capacity = parseCapacityInput(draftCapacity);
    const publishBatchValid = isValidPublishBatchInput(draftPublishBatch);
    const nameTaken = rings.some((r) => r.name === trimmedName);
    const canCreate = trimmedName.length > 0 && capacity !== null && publishBatchValid && !nameTaken && !busy;
    const createTitle = trimmedName === ''
        ? 'Type a ring name to create it'
        : nameTaken
            ? `Ring "${trimmedName}" already exists`
            : capacity === null
                ? `Capacity must be a power of two from ${RING_MIN_CAPACITY} to ${RING_MAX_CAPACITY} bytes`
                : !publishBatchValid
                    ? `Publish batch must be 0 to ${RING_MAX_PUBLISH_BATCH}`
                    : `Create ring "${trimmedName}"`;

    // Clears the draft only once the service confirms the ring exists; a
    // rejected create (name taken, bad capacity) leaves the fields as the
    // user typed them, so correcting and retrying does not mean retyping.
    const handleCreate = async (): Promise<void> => {
        if (!canCreate || capacity === null) return;
        const created = await onCreate(trimmedName, Number(capacity), resolvePublishBatchForSubmit(draftPublishBatch));
        if (created) {
            setDraftName('');
            setDraftCapacity('');
            setDraftPublishBatch('');
        }
    };

    return (
        <div className="pdump-rings-panel">
            <div className="pdump-rings-panel__header">
                <span className="pdump-rings-panel__title">Rings</span>
            </div>

            <div className="pdump-rings-panel__create">
                <TextInput
                    size="s"
                    value={draftName}
                    onUpdate={setDraftName}
                    placeholder="ring name"
                    disabled={busy}
                />
                <TextInput
                    size="s"
                    type="number"
                    value={draftCapacity}
                    onUpdate={setDraftCapacity}
                    placeholder={`capacity bytes, power of two (${RING_MIN_CAPACITY}–${RING_MAX_CAPACITY})`}
                    error={draftCapacity !== '' && capacity === null ? 'power of two' : undefined}
                    disabled={busy}
                />
                <TextInput
                    size="s"
                    type="number"
                    value={draftPublishBatch}
                    onUpdate={setDraftPublishBatch}
                    placeholder={`publish batch, 0–${RING_MAX_PUBLISH_BATCH} (default ${RING_DEFAULT_PUBLISH_BATCH})`}
                    error={!publishBatchValid ? `0–${RING_MAX_PUBLISH_BATCH}` : undefined}
                    disabled={busy}
                />
                <button
                    type="button"
                    className="yn-table-action-btn"
                    title={createTitle}
                    aria-label={createTitle}
                    disabled={!canCreate}
                    onClick={() => { void handleCreate(); }}
                >
                    <Icon data={Plus} size={16} />
                </button>
            </div>

            {loading ? (
                <div className="pdump-rings-panel__empty">Loading rings…</div>
            ) : rings.length === 0 ? (
                <div className="pdump-rings-panel__empty">No rings registered.</div>
            ) : (
                <ul className="pdump-rings-panel__list">
                    {rings.map((r) => (
                        <li key={r.name} className="pdump-rings-panel__row">
                            <span className="pdump-rings-panel__name">{r.name}</span>
                            <span className="pdump-rings-panel__meta">
                                {formatBytes(ringCapacityBigInt(r))} / worker
                            </span>
                            <span className="pdump-rings-panel__meta">
                                batch {r.publish_batch ?? RING_DEFAULT_PUBLISH_BATCH}
                            </span>
                            <button
                                type="button"
                                className="yn-table-action-btn yn-table-action-btn--delete"
                                title={`Delete ring "${r.name}"`}
                                aria-label={`Delete ring "${r.name}"`}
                                disabled={busy || !r.name}
                                onClick={() => r.name && onDeleteRequest(r.name)}
                            >
                                <TrashIcon />
                            </button>
                        </li>
                    ))}
                </ul>
            )}
        </div>
    );
};

export default React.memo(RingsPanel);
