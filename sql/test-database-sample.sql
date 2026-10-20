-- Puff sample dataset for PostgreSQL database "test".
-- Open this file in Puff, use a writable connection to test, and run the entire file.
-- This is ONE SQL statement, compatible with Puff's single-statement executor.
-- Creates only the dedicated puff_sample schema: 10 related tables, a view,
-- indexes, JSON, arrays, Unicode text, NULLs, a generated column and composite keys.
-- Roughly 16,000 rows, enough to test paging without a huge import.
-- Requires CREATE permission on the database. No extensions or superuser required.
-- Existing tables are never dropped or overwritten. If puff_sample already exists,
-- execution stops without changing it. Any failure rolls back the whole import.

DO $puff_seed$
BEGIN
  IF current_database() <> 'test' THEN
    RAISE EXCEPTION 'Connect to database test first (currently connected to %).', current_database();
  END IF;
  IF EXISTS (SELECT 1 FROM pg_namespace WHERE nspname = 'puff_sample') THEN
    RAISE EXCEPTION 'Schema puff_sample already exists. No sample data was changed.';
  END IF;

  CREATE SCHEMA puff_sample;

SET LOCAL search_path TO puff_sample, public;

CREATE TABLE IF NOT EXISTS departments (
  id bigint PRIMARY KEY,
  name varchar(100) NOT NULL UNIQUE,
  budget numeric(14,2) NOT NULL CHECK (budget >= 0),
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS employees (
  id bigint PRIMARY KEY,
  department_id bigint NOT NULL REFERENCES departments(id),
  manager_id bigint REFERENCES employees(id),
  first_name text NOT NULL,
  last_name text NOT NULL,
  email text NOT NULL UNIQUE,
  salary numeric(12,2) NOT NULL CHECK (salary >= 0),
  hired_on date NOT NULL,
  active boolean NOT NULL DEFAULT true,
  skills text[] NOT NULL DEFAULT '{}',
  metadata jsonb NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS customers (
  id bigint PRIMARY KEY,
  full_name text NOT NULL,
  email text NOT NULL UNIQUE,
  phone text,
  city text NOT NULL,
  country_code char(2) NOT NULL,
  birth_date date,
  loyalty_points integer NOT NULL DEFAULT 0 CHECK (loyalty_points >= 0),
  active boolean NOT NULL DEFAULT true,
  preferences jsonb NOT NULL DEFAULT '{}',
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS categories (
  id bigint PRIMARY KEY,
  name text NOT NULL UNIQUE,
  description text
);

CREATE TABLE IF NOT EXISTS products (
  id bigint PRIMARY KEY,
  category_id bigint NOT NULL REFERENCES categories(id),
  sku text NOT NULL UNIQUE,
  name text NOT NULL,
  description text,
  price numeric(12,2) NOT NULL CHECK (price >= 0),
  stock integer NOT NULL CHECK (stock >= 0),
  weight_kg double precision,
  available boolean NOT NULL DEFAULT true,
  tags text[] NOT NULL DEFAULT '{}',
  attributes jsonb NOT NULL DEFAULT '{}',
  created_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS orders (
  id bigint PRIMARY KEY,
  customer_id bigint NOT NULL REFERENCES customers(id),
  employee_id bigint NOT NULL REFERENCES employees(id),
  status text NOT NULL CHECK (status IN ('pending','paid','shipped','delivered','cancelled')),
  shipping_address jsonb NOT NULL,
  notes text,
  ordered_at timestamptz NOT NULL,
  shipped_at timestamptz,
  CHECK (shipped_at IS NULL OR shipped_at >= ordered_at)
);

-- Composite primary key exercises Puff's targeted row editing.
CREATE TABLE IF NOT EXISTS order_items (
  order_id bigint NOT NULL REFERENCES orders(id) ON DELETE CASCADE,
  line_number smallint NOT NULL,
  product_id bigint NOT NULL REFERENCES products(id),
  quantity integer NOT NULL CHECK (quantity > 0),
  unit_price numeric(12,2) NOT NULL CHECK (unit_price >= 0),
  discount_percent numeric(5,2) NOT NULL DEFAULT 0 CHECK (discount_percent BETWEEN 0 AND 100),
  line_total numeric(14,2) GENERATED ALWAYS AS
    (round(quantity * unit_price * (1 - discount_percent / 100), 2)) STORED,
  PRIMARY KEY (order_id, line_number)
);

CREATE TABLE IF NOT EXISTS payments (
  id bigint PRIMARY KEY,
  order_id bigint NOT NULL UNIQUE REFERENCES orders(id),
  amount numeric(14,2) NOT NULL CHECK (amount >= 0),
  method text NOT NULL CHECK (method IN ('card','bank_transfer','cash','wallet')),
  reference text NOT NULL UNIQUE,
  paid_at timestamptz NOT NULL
);

CREATE TABLE IF NOT EXISTS reviews (
  id bigint PRIMARY KEY,
  product_id bigint NOT NULL REFERENCES products(id),
  customer_id bigint NOT NULL REFERENCES customers(id),
  rating smallint NOT NULL CHECK (rating BETWEEN 1 AND 5),
  title text,
  body text NOT NULL,
  verified boolean NOT NULL DEFAULT false,
  created_at timestamptz NOT NULL
);

CREATE TABLE IF NOT EXISTS audit_events (
  id bigint PRIMARY KEY,
  employee_id bigint REFERENCES employees(id),
  action text NOT NULL,
  entity_type text NOT NULL,
  entity_id bigint NOT NULL,
  payload jsonb NOT NULL,
  ip_address inet,
  occurred_at timestamptz NOT NULL
);

-- Additional columns demonstrate additive schema changes.
ALTER TABLE customers ADD COLUMN IF NOT EXISTS nickname text;
ALTER TABLE customers ADD COLUMN IF NOT EXISTS last_login_at timestamptz;
ALTER TABLE products ADD COLUMN IF NOT EXISTS discontinued_at timestamptz;
ALTER TABLE employees ADD COLUMN IF NOT EXISTS emergency_contact jsonb;

INSERT INTO departments (id, name, budget)
SELECT n, (ARRAY['Engineering','Sales','Support','Finance','Operations','Marketing','People','Research'])[n],
       5000 + n * 35000
FROM generate_series(1,8) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO employees (id, department_id, manager_id, first_name, last_name, email,
                       salary, hired_on, active, skills, metadata, emergency_contact)
SELECT n, 1 + (n - 1) % 8, NULL,
       (ARRAY['Ali','Sara','Ahmed','Ayesha','Omar','Fatima','Hassan','Noor'])[1 + (n - 1) % 8],
       'Employee ' || n, 'employee' || n || '@example.test',
       40000 + (n * 137) % 140000,
       DATE '2018-01-01' + (n % 2400)::integer,
       n % 17 <> 0,
       ARRAY['PostgreSQL', CASE WHEN n % 2 = 0 THEN 'C++' ELSE 'Design' END],
       jsonb_build_object('remote', n % 3 = 0, 'level', 1 + n % 6),
       CASE WHEN n % 5 = 0 THEN NULL ELSE jsonb_build_object('name','Contact ' || n,'phone','+92-300-0000000') END
FROM generate_series(1,250) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO customers (id, full_name, email, phone, city, country_code, birth_date,
                       loyalty_points, active, preferences, created_at, nickname, last_login_at)
SELECT n, 'Customer ' || n, 'customer' || n || '@example.test',
       CASE WHEN n % 7 = 0 THEN NULL ELSE '+92-' || lpad(n::text,10,'0') END,
       (ARRAY['Karachi','Lahore','Islamabad','Peshawar','Quetta','Multan'])[1 + (n - 1) % 6],
       'PK', DATE '1965-01-01' + (n % 18000)::integer,
       (n * 13) % 1000, n % 19 <> 0,
       jsonb_build_object('theme',CASE WHEN n % 2 = 0 THEN 'dark' ELSE 'light' END,
                          'newsletter',n % 3 = 0,'language','en'),
       TIMESTAMPTZ '2024-01-01 00:00:00+00' + n * interval '5 minutes',
       CASE WHEN n % 4 = 0 THEN 'Guest ' || n ELSE NULL END,
       CASE WHEN n % 9 = 0 THEN NULL ELSE TIMESTAMPTZ '2026-01-01 00:00:00+00' + n * interval '1 minute' END
FROM generate_series(1,1000) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO categories (id,name,description)
SELECT n, (ARRAY['Computers','Phones','Audio','Accessories','Office','Home','Books','Gaming','Cameras','Networking'])[n],
       'Demo category ' || n
FROM generate_series(1,10) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO products (id,category_id,sku,name,description,price,stock,weight_kg,available,tags,attributes)
SELECT n, 1 + (n - 1) % 10, 'PUFF-' || lpad(n::text,6,'0'),
       'Demo Product ' || n,
       CASE WHEN n % 11 = 0 THEN NULL ELSE 'A sample product for testing inline edits, filters, and exports.' END,
       round((500 + (n * 791) % 250000)::numeric / 100,2),
       (n * 17) % 500, round((n % 200)::numeric / 10,2)::double precision,
       n % 23 <> 0, ARRAY['demo',CASE WHEN n % 2 = 0 THEN 'popular' ELSE 'new' END],
       jsonb_build_object('color',(ARRAY['black','white','blue','green'])[1 + n % 4],
                          'warranty_months',12 + n % 3 * 12)
FROM generate_series(1,200) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO orders (id,customer_id,employee_id,status,shipping_address,notes,ordered_at,shipped_at)
SELECT n, 1 + (n * 37 - 1) % 1000, 1 + (n - 1) % 250,
       (ARRAY['pending','paid','shipped','delivered','cancelled'])[1 + (n - 1) % 5],
       jsonb_build_object('street',n || ' Demo Street','city','Karachi','postal_code','74000','country','PK'),
       CASE WHEN n % 10 = 0 THEN E'Handle with care.\nCustomer requested a phone call.' ELSE NULL END,
       TIMESTAMPTZ '2025-01-01 00:00:00+00' + n * interval '10 minutes',
       CASE WHEN (n - 1) % 5 IN (2,3) THEN
         TIMESTAMPTZ '2025-01-01 00:00:00+00' + n * interval '10 minutes' + interval '2 days'
       ELSE NULL END
FROM generate_series(1,1500) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO order_items (order_id,line_number,product_id,quantity,unit_price,discount_percent)
SELECT o.id, l.n, p.id, 1 + (o.id + l.n) % 5, p.price,
       CASE WHEN o.id % 7 = 0 THEN 10 ELSE 0 END
FROM orders o
CROSS JOIN generate_series(1,4) AS l(n)
JOIN products p ON p.id = 1 + (o.id * 13 + l.n * 7) % 200
WHERE o.id BETWEEN 1 AND 1500
ON CONFLICT (order_id,line_number) DO NOTHING;

INSERT INTO payments (id,order_id,amount,method,reference,paid_at)
SELECT o.id,o.id,sum(i.line_total),
       (ARRAY['card','bank_transfer','cash','wallet'])[1 + o.id % 4],
       'PAY-' || lpad(o.id::text,8,'0'),o.ordered_at + interval '15 minutes'
FROM orders o JOIN order_items i ON i.order_id = o.id
WHERE o.status IN ('paid','shipped','delivered') AND o.id BETWEEN 1 AND 1500
GROUP BY o.id,o.ordered_at
ON CONFLICT (id) DO NOTHING;

INSERT INTO reviews (id,product_id,customer_id,rating,title,body,verified,created_at)
SELECT n,1 + (n * 17 - 1) % 200,1 + (n * 31 - 1) % 1000,
       1 + (n - 1) % 5,'Review ' || n,
       CASE WHEN n % 3 = 0 THEN E'Works well.\nPackaging was excellent. Unicode: café — پاکستان'
            ELSE 'Useful demo review with commas, "quotes", and ordinary text.' END,
       n % 4 <> 0,TIMESTAMPTZ '2025-06-01 00:00:00+00' + n * interval '7 minutes'
FROM generate_series(1,1000) AS g(n)
ON CONFLICT (id) DO NOTHING;

INSERT INTO audit_events (id,employee_id,action,entity_type,entity_id,payload,ip_address,occurred_at)
SELECT n,CASE WHEN n % 13 = 0 THEN NULL ELSE 1 + (n - 1) % 250 END,
       (ARRAY['create','update','view','export'])[1 + (n - 1) % 4],
       'order',1 + (n - 1) % 1500,
       jsonb_build_object('source','demo-seed','request_id','req-' || n,'duration_ms',n % 500),
       ('10.0.' || ((n / 254) % 254) || '.' || (1 + n % 254))::inet,
       TIMESTAMPTZ '2026-01-01 00:00:00+00' + n * interval '10 seconds'
FROM generate_series(1,5000) AS g(n)
ON CONFLICT (id) DO NOTHING;

CREATE INDEX IF NOT EXISTS employees_department_idx ON employees(department_id);
CREATE INDEX IF NOT EXISTS employees_manager_idx ON employees(manager_id);
CREATE INDEX IF NOT EXISTS customers_city_idx ON customers(city);
CREATE INDEX IF NOT EXISTS products_category_idx ON products(category_id);
CREATE INDEX IF NOT EXISTS orders_customer_date_idx ON orders(customer_id,ordered_at DESC);
CREATE INDEX IF NOT EXISTS orders_employee_idx ON orders(employee_id);
CREATE INDEX IF NOT EXISTS orders_status_idx ON orders(status);
CREATE INDEX IF NOT EXISTS order_items_product_idx ON order_items(product_id);
CREATE INDEX IF NOT EXISTS reviews_product_idx ON reviews(product_id);
CREATE INDEX IF NOT EXISTS reviews_customer_idx ON reviews(customer_id);
CREATE INDEX IF NOT EXISTS audit_events_employee_idx ON audit_events(employee_id);
CREATE INDEX IF NOT EXISTS audit_events_time_idx ON audit_events(occurred_at DESC);
CREATE INDEX IF NOT EXISTS products_attributes_idx ON products USING gin(attributes);

CREATE OR REPLACE VIEW order_summary AS
SELECT o.id,o.customer_id,c.full_name AS customer,o.status,o.ordered_at,
       count(i.line_number) AS item_lines,coalesce(sum(i.line_total),0) AS total
FROM orders o JOIN customers c ON c.id = o.customer_id
LEFT JOIN order_items i ON i.order_id = o.id
GROUP BY o.id,c.full_name;

END
$puff_seed$;

-- After completion, click Reload schemas and expand puff_sample.
-- Run these examples ONE AT A TIME in a separate editor query:
-- SELECT * FROM puff_sample.customers ORDER BY id;
-- SELECT * FROM puff_sample.order_summary ORDER BY total DESC LIMIT 100;
-- SELECT status, count(*) FROM puff_sample.orders GROUP BY status ORDER BY status;
-- SELECT * FROM puff_sample.products WHERE attributes @> '{"color":"blue"}';
-- SELECT * FROM puff_sample.order_items ORDER BY order_id, line_number;
