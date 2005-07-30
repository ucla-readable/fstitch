import java.util.HashMap;
import java.util.Iterator;

public class HashMapValueIterator implements Iterator
{
	private HashMap map;
	private Iterator keys;
	
	public HashMapValueIterator(HashMap map)
	{
		this.map = map;
		keys = map.keySet().iterator();
	}
	
	public boolean hasNext()
	{
		return keys.hasNext();
	}
	
	public Object next()
	{
		return map.get(keys.next());
	}
	
	public void remove()
	{
		keys.remove();
	}
}
