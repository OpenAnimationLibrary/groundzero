"""Qualify real evaluator calls and Python callback error propagation in CI."""
import SeExprPy as expr


def require(condition, message):
    if not condition:
        raise AssertionError(message)


require(abs(expr.evaluate('sin($u)', {'u': 0.5}, 'scalar') - 0.479425538604203) < 1e-12,
        'native scalar evaluation failed')
require(expr.evaluate('[$u,2,3,4]', {'u': 1.0}, 'vector', 4) == (1.0, 2.0, 3.0, 4.0),
        'native vector dimension was lost')
require(expr.evaluate('"é"', result_type='string') == 'é', 'native string evaluation failed')
require(expr.validate('1+', result_type='scalar')['valid'] is False,
        'native invalid expression was accepted')
require(bool(expr.validate('1+', result_type='scalar')['errors']), 'native diagnostics were omitted')
catalog = {item['name']: item for item in expr.functions()}
require(len(catalog) == 99 and bool(catalog['fbm']['documentation']), 'actual function catalog is incomplete')
callback = {'twice': {'arguments': ['scalar'], 'result_type': 'scalar',
                      'callback': lambda value: value * 2}}
require(expr.evaluate('twice($x)', {'x': 3.5}, 'scalar', functions=callback) == 7,
        'Python host callback did not execute')
vector_callback = {'offset': {'arguments': ['vector'], 'argument_dimensions': [4],
                              'result_type': 'vector', 'dimension': 4,
                              'callback': lambda value: tuple(item + 1 for item in value)}}
require(expr.evaluate('offset($input)', {'input': [1, 2, 3, 4]}, 'vector', 4,
                      functions=vector_callback) == (2.0, 3.0, 4.0, 5.0),
        'Python vector callback dimension was lost')
single_callback = {'offset': {'arguments': ['vector'], 'argument_dimensions': [1],
                              'result_type': 'vector', 'dimension': 1,
                              'callback': lambda value: (value[0] + 1,)}}
require(expr.evaluate('offset($input)', {'input': [1]}, 'vector', 1,
                      functions=single_callback) == (2.0,),
        'declared one-component vectors were changed into scalars at the callback boundary')


def fail(_value):
    raise ValueError('fixture callback error')


try:
    expr.evaluate('twice($x)', {'x': 1}, 'scalar', functions={
        'twice': {'arguments': ['scalar'], 'result_type': 'scalar', 'callback': fail}})
except RuntimeError as error:
    require('fixture callback error' in str(error), 'callback exception detail was lost')
else:
    raise AssertionError('callback failure returned a successful value')
require(expr.evaluate('2+3', result_type='scalar') == 5, 'evaluation did not recover after callback failure')
print('SeExpr Python evaluation: PASS')
