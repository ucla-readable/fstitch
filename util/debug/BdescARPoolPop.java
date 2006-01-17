import java.io.DataInput;
import java.io.IOException;

public class BdescARPoolPop extends Opcode
{
	private final int depth;
	
	public BdescARPoolPop(int depth)
	{
		this.depth = depth;
	}
	
	public void applyTo(SystemState state)
	{
	}
	
	public String toString()
	{
		return "KDB_BDESC_AR_POOL_POP: depth = " + depth;
	}
	
	public static ModuleOpcodeFactory getFactory(DataInput input)
	{
		ModuleOpcodeFactory factory = new ModuleOpcodeFactory(input, KDB_BDESC_AR_POOL_POP, "KDB_BDESC_AR_POOL_POP", BdescARPoolPop.class);
		factory.addParameter("depth", 4);
		return factory;
	}
}
