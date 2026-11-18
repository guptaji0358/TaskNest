"""account tables (CLOUD.md 16.6): users, devices, refresh_families, refresh_tokens, access_tokens

Revision ID: 0001
Revises:
"""
from alembic import op
import sqlalchemy as sa

revision = "0001"
down_revision = None
branch_labels = None
depends_on = None

_uuid = sa.Uuid(as_uuid=True)
_ts = sa.DateTime(timezone=True)


def upgrade() -> None:
    op.create_table(
        "users",
        sa.Column("id", _uuid, nullable=False),
        sa.Column("login", sa.Text, nullable=False),
        sa.Column("password_hash", sa.Text, nullable=False),
        sa.Column("created_at", _ts, nullable=False, server_default=sa.func.now()),
        sa.PrimaryKeyConstraint("id", name="pk_users"),
        sa.UniqueConstraint("login", name="uq_users_login"),
    )
    op.create_table(
        "devices",
        sa.Column("id", _uuid, nullable=False),
        sa.Column("account_id", _uuid, nullable=False),
        sa.Column("name", sa.Text, nullable=False),
        sa.Column("platform", sa.Text, nullable=False),
        sa.Column("client_version", sa.Text, nullable=False),
        sa.Column("created_at", _ts, nullable=False),
        sa.Column("last_seen_at", _ts, nullable=False),
        sa.PrimaryKeyConstraint("id", name="pk_devices"),
        sa.ForeignKeyConstraint(["account_id"], ["users.id"], name="fk_devices_account_id_users", ondelete="CASCADE"),
    )
    op.create_index("ix_devices_account_id", "devices", ["account_id"])
    op.create_table(
        "refresh_families",
        sa.Column("id", _uuid, nullable=False),
        sa.Column("account_id", _uuid, nullable=False),
        sa.Column("device_id", _uuid, nullable=False),
        sa.Column("created_at", _ts, nullable=False),
        sa.Column("absolute_expires_at", _ts, nullable=False),
        sa.Column("sliding_expires_at", _ts, nullable=False),
        sa.Column("revoked_at", _ts),
        sa.Column("revoked_reason", sa.Text),
        sa.PrimaryKeyConstraint("id", name="pk_refresh_families"),
        sa.ForeignKeyConstraint(["account_id"], ["users.id"], name="fk_refresh_families_account_id_users", ondelete="CASCADE"),
        sa.ForeignKeyConstraint(["device_id"], ["devices.id"], name="fk_refresh_families_device_id_devices", ondelete="CASCADE"),
        sa.UniqueConstraint("device_id", name="uq_refresh_families_device_id"),
        sa.UniqueConstraint("id", "account_id", name="uq_refresh_families_id_account_id"),
    )
    op.create_index("ix_refresh_families_account_id", "refresh_families", ["account_id"])
    op.create_table(
        "refresh_tokens",
        sa.Column("id", _uuid, nullable=False),
        sa.Column("family_id", _uuid, nullable=False),
        sa.Column("generation", sa.Integer, nullable=False),
        sa.Column("token_hash", sa.LargeBinary(32), nullable=False),
        sa.Column("created_at", _ts, nullable=False),
        sa.Column("rotated_at", _ts),
        sa.Column("rotation_operation_id", _uuid),
        sa.PrimaryKeyConstraint("id", name="pk_refresh_tokens"),
        sa.ForeignKeyConstraint(["family_id"], ["refresh_families.id"], name="fk_refresh_tokens_family_id_refresh_families", ondelete="CASCADE"),
        sa.UniqueConstraint("token_hash", name="uq_refresh_tokens_token_hash"),
        sa.UniqueConstraint("family_id", "rotation_operation_id", name="uq_refresh_tokens_family_id_rotation_operation_id"),
    )
    op.create_index("ix_refresh_tokens_family_rotated", "refresh_tokens", ["family_id", "rotated_at"],
                    postgresql_where=sa.text("rotated_at IS NOT NULL"))
    op.create_table(
        "access_tokens",
        sa.Column("id", _uuid, nullable=False),
        sa.Column("token_hash", sa.LargeBinary(32), nullable=False),
        sa.Column("family_id", _uuid, nullable=False),
        sa.Column("account_id", _uuid, nullable=False),
        sa.Column("issued_at", _ts, nullable=False),
        sa.Column("expires_at", _ts, nullable=False),
        sa.PrimaryKeyConstraint("id", name="pk_access_tokens"),
        sa.UniqueConstraint("token_hash", name="uq_access_tokens_token_hash"),
        sa.ForeignKeyConstraint(["family_id", "account_id"], ["refresh_families.id", "refresh_families.account_id"],
                                name="fk_access_tokens_family_account", ondelete="CASCADE"),
    )
    op.create_index("ix_access_tokens_family_id", "access_tokens", ["family_id"])


def downgrade() -> None:
    for table in ("access_tokens", "refresh_tokens", "refresh_families", "devices", "users"):
        op.drop_table(table)
