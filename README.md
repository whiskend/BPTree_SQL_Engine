# B+Tree SQL Engine

## 전체 구조

![B+Tree SQL Engine 단순 구조도](docs/architecture_no_runtime.svg)

## B+Tree의 장점

### 1. 검색 성능이 안정적이다

![alt text](image.png)
B+Tree는 전체 데이터를 처음부터 끝까지 확인하지 않고, 트리 구조를 따라 내려가며 key를 찾습니다.

```text
전체 스캔: O(N)
B+Tree 검색: O(log N)
```

데이터가 많아질수록 전체 스캔보다 유리합니다.

### 2. 정렬된 key를 유지한다

![alt text](image-1.png)

B+Tree는 key를 정렬된 상태로 유지합니다.

그래서 단순 조회뿐 아니라 다음과 같은 작업에도 유리합니다.

```sql
SELECT * FROM users WHERE id BETWEEN 10 AND 100;
SELECT * FROM users ORDER BY id;
```

Hash Table은 단일 key 조회에는 빠를 수 있지만, key의 정렬 순서를 유지하지 않기 때문에 범위 검색이나 정렬에는 약합니다.


### 3. 디스크 기반 DB에 잘 맞는다

![alt text](image-2.png)
실제 DB는 데이터를 디스크나 SSD의 페이지 단위로 읽습니다.

B+Tree는 노드 하나에 여러 key를 담기 때문에 트리 높이가 낮고, 디스크 접근 횟수를 줄이는 데 유리합니다.

```text
트리 높이가 낮음
-> 디스크 접근 횟수 감소
-> DB 인덱스에 적합
```

이렇게 하면 인덱스는 row의 위치만 빠르게 찾고, 실제 row 읽기는 storage 계층이 담당합니다.

## B+Tree의 한계

### 1. 인덱스 유지 비용이 있다

![alt text](image-3.png)

B+Tree는 조회를 빠르게 해주지만, INSERT 때는 추가 작업이 필요합니다.

```text
.data 파일에 row 저장
+ B+Tree에 id -> row_offset 삽입
```

즉 인덱스가 없는 경우보다 쓰기 비용이 증가할 수 있습니다.

### 2. 노드 split 비용이 있다

B+Tree 노드가 가득 차면 split이 발생합니다.

```text
leaf node split
internal node split
parent key 갱신
root 변경 가능
```

대부분의 경우 안정적으로 동작하지만, 대량 INSERT에서는 split 비용이 누적될 수 있습니다.

### 3. 추가 메모리나 디스크 공간이 필요하다
![alt text](image-4.png)

인덱스는 원본 데이터와 별도로 저장됩니다.

```text
원본 데이터
+ B+Tree 노드
+ key 배열
+ child pointer 또는 row_offset
```
