public class MissingParameterException extends BadInputException
{
	private final String name;
	private final int size;
	
	public MissingParameterException(String name, int size)
	{
		super("Missing parameter: \"" + name + "\" of size " + size);
		this.name = name;
		this.size = size;
	}
	
	public String getName()
	{
		return name;
	}
	
	public int  getSize()
	{
		return size;
	}
}
